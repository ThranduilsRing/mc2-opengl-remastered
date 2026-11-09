// Terrain: Terrain::init (mission terrain load), the block / mover list helpers it uses.
// Declared in the class headers; shared names in terrain_internal.h.
#include "mc2_czstring.h"
#include <algorithm>
#include <chrono>
#include <string>
#include "terrain_internal.h"
#include "mc2fmt.h"
#include "diagnostic_trace.h"
#include "terrain_runtime.h"
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <span>
#include <string_view>
#include <vector>
#include "../GameOS/gameos/terrain_field_store.h"
#include "../GameOS/gameos/height_sig.h"


//---------------------------------------------------------------------------
// Per-vertex heightfield and material copies the terrain LOD-chunk load phases of Terrain::init share.
struct TerrainLodInitFrame {
	std::vector<float> elev;
	std::vector<float> ttype;
};

// TERRAIN-BEAUTY-SIG-1: one gameplay-height signature verdict per mission load. <beauty>/gameplay_height.sig =
// "<mapSide> <fnv1a64 hex>" over (mapSide u32, then mapSide^2 float32 elevations). 0 no sig file, 1 match, 2 mismatch.
// A mismatch means the <stem>.beauty sidecars belong to different terrain (a mod under a stock stem): the
// displacement bake, the shoreline mask, the shore field and the water-shore height read all skip.
namespace {
struct BeautySigVerdict {
	int state = 0;
	char got[96] = "";
	char want[96] = "";
};
BeautySigVerdict s_beautySig;

BeautySigVerdict computeBeautySig(std::string_view terrainName, int mapSide, const std::vector<float>& elev)
{
	BeautySigVerdict v;
	if (mapSide < 2 || elev.size() != static_cast<size_t>(mapSide) * static_cast<size_t>(mapSide)) return v;
	std::string sigPath;
	if (mc2::gate::isSet<"MC2_TERRAIN_VISUAL_HEIGHT_FILE">())
	{
		const std::filesystem::path vhFile(mc2::gate::str<"MC2_TERRAIN_VISUAL_HEIGHT_FILE">());
		if (!vhFile.has_parent_path()) return v;
		sigPath = std::filesystem::path(vhFile).replace_filename("gameplay_height.sig").string();
	}
	else
	{
		std::string stem(terrainName);
		const auto slash = stem.find_last_of("/\\");
		if (slash != std::string::npos) stem.erase(0, slash + 1);
		if (const auto dot = stem.find_last_of('.'); dot != std::string::npos) stem.erase(dot);
		sigPath = "data/missions/" + stem + ".beauty/gameplay_height.sig";
	}
	if (FILE* sg = fopen(sigPath.c_str(), "rb"))   // path-audit: loose-only (sidecar beside the beauty bake)
	{
		char line[96] = "";
		if (fgets(line, sizeof(line), sg))
		{
			size_t ll = strlen(line);
			while (ll && (line[ll - 1] == '\n' || line[ll - 1] == '\r' || line[ll - 1] == ' ')) line[--ll] = '\0';
			const std::string want = mc2terrain::gameplayHeightSig(mapSide, elev);
			strncpy(v.want, want.c_str(), sizeof(v.want) - 1);
			strncpy(v.got, line, sizeof(v.got) - 1);
			v.state = (strcmp(v.got, v.want) == 0) ? 1 : 2;
		}
		fclose(sg);
	}
	return v;
}
}  // namespace


// Derive the map extents and block counts from the vertex count.
void Terrain::initMapExtents(unsigned long verticesPerMapSide, unsigned long visibleVertices) {
	realVerticesMapSide = verticesPerMapSide;
	halfVerticesMapSide = realVerticesMapSide >> 1;
	blocksMapSide = realVerticesMapSide / verticesBlockSide;
	worldUnitsMapSide = realVerticesMapSide * worldUnitsPerVertex;
	if (worldUnitsMapSide > Stuff::SMALL)
		oneOverWorldUnitsMapSide = 1.0f / worldUnitsMapSide;
	else
		oneOverWorldUnitsMapSide = 0.0f;

	// Tell GameOS the map extent for static shadow projection
	gos_SetMapHalfExtent(worldUnitsMapSide * 0.5f);

	Terrain::numObjBlocks = blocksMapSide * blocksMapSide;
	visibleVerticesPerSide = visibleVertices;
	terrainHeapSize = MAX_TERRAIN_HEAP_SIZE;
}

// Create the terrain heap on first load.
void Terrain::initTerrainHeap(volatile float& percent, float percentRange) {
	//-----------------------------------------------------------------
	// Startup to Terrain Heap


	percent += percentRange/5.f;
}

// Create the terrain texture set on first load.
void Terrain::initTerrainTextures(PacketFile* pakFile, volatile float& percent, float percentRange) {
	//-----------------------------------------------------------------
	// Startup the Terrain Texture Maps
	if ( !terrainTextures )
	{
		ZoneScopedN("Terrain::init terrainTextures");
		std::string baseName;
		if (pakFile)
		{
			baseName = std::filesystem::path(pakFile->getFilename()).stem().string();
		}
		else
		{
			baseName = "newmap";
		}

		terrainTexturesOwner = std::make_unique<TerrainTextures>();
		terrainTextures = terrainTexturesOwner.get();
		terrainTextures->init("textures",baseName.c_str());
	}

	percent += percentRange/5.f;
}

// Pick the colour-map name and create the colour map when its image exists.
void Terrain::initColorMapTexture(PacketFile* pakFile, volatile float& percent, float percentRange) {
	//-----------------------------------------------------------------
	// Startup the Terrain Color Map
	if ( !terrainTextures2 && pakFile)
	{
		ZoneScopedN("Terrain::init terrainColorMap");

		std::string name = std::filesystem::path(pakFile->getFilename()).stem().string();
		terrainNameOwner = std::make_unique<char[]>(name.size()+1);
		strcpy(terrainNameOwner.get(),name.c_str());
		terrainName = terrainNameOwner.get();

		if (colorMapName)
			name = colorMapName;

		FullPathFileName tgaColorMapName;
		tgaColorMapName.init(texturePath,name.c_str(),".tga");
		
		FullPathFileName tgaColorMapBurninName;
		tgaColorMapBurninName.init(texturePath,name.c_str(),".burnin.tga");

		FullPathFileName tgaColorMapJPGName;
		tgaColorMapJPGName.init(texturePath,name.c_str(),".burnin.jpg");
				
		if (fileExists(tgaColorMapName) || fileExists(tgaColorMapBurninName) || fileExists(tgaColorMapJPGName))
		{
			terrainTextures2Owner = std::make_unique<TerrainColorMap>();
			terrainTextures2 = terrainTextures2Owner.get();		//Otherwise, this will stay NULL and we know not to use them
		}
	}

	percent += percentRange/5.f;
}

// Allocate the object-block, active-vertex and mover / used block lists.
void Terrain::initObjBlockLists(volatile float& percent, float percentRange) {
	mapTopLeft3d.x = -worldUnitsMapSide / 2.0f;
	mapTopLeft3d.y = worldUnitsMapSide / 2.0f;

	percent += percentRange/5.f;


	//----------------------------------------------------------------------
	// Setup number of blocks
	long numberBlocks = blocksMapSide * blocksMapSide;
	
	numObjBlocks = numberBlocks;
	objBlockInfo = static_cast<ObjBlockInfo *>(std::malloc(sizeof(ObjBlockInfo)*numObjBlocks));
	gosASSERT(objBlockInfo != nullptr);
	
	memset(objBlockInfo,0,sizeof(ObjBlockInfo)*numObjBlocks);
	
	objVertexActive = static_cast<bool *>(std::malloc(sizeof(bool) * realVerticesMapSide * realVerticesMapSide));
	gosASSERT(objVertexActive != nullptr);
	
	memset(objVertexActive,0,sizeof(bool)*numObjBlocks);
	
	moverBlockList = static_cast<long *>(std::malloc(sizeof(long) * numberBlocks));
	gosASSERT(moverBlockList != nullptr);
	
	usedBlockList = static_cast<long *>(std::malloc(sizeof(long) * numberBlocks));
	gosASSERT(usedBlockList != nullptr);
	
	clearList();
	clearMoverList();
}

// Create the map data manager and upload the height texture.
void Terrain::initMapData(PacketFile* pakFile, volatile float& percent, float percentRange) {
	//----------------------------------------------------------------------
	// Calculate size of each mapblock
	long blockSize = verticesBlockSide * verticesBlockSide;
	blockSize *= sizeof(PostcompVertex);

	//----------------------------------------------------------------------
	// Create the MapBlock Manager and allocate its RAM
	if ( !mapData )
	{
		mapDataOwner = std::make_unique<MapData>();
		mapData = mapDataOwner.get();
		if ( pakFile )
			mapData->newInit( pakFile, realVerticesMapSide*realVerticesMapSide);
		else
			mapData->newInit( realVerticesMapSide*realVerticesMapSide );

		mapTopLeft3d.z = mapData->getTopLeftElevation();


	}

	percent += percentRange/5.f;
}

// LOD chunk: allocate and initialise the block and superchunk metadata.
void Terrain::lodAllocMeta() {
	// terrainChunkSide = ceil((vertices-1) / verticesBlockSide)
	s_terrainChunkSide = ((realVerticesMapSide - 1) + verticesBlockSide - 1) / verticesBlockSide;
	s_superchunkSide   = (s_terrainChunkSide + 3) / 4;

	int nBlocks      = s_terrainChunkSide * s_terrainChunkSide;
	int nSuperchunks = s_superchunkSide   * s_superchunkSide;

	s_blockMeta      = static_cast<TerrainBlockMeta*>(std::malloc(sizeof(TerrainBlockMeta)   * nBlocks));
	s_superchunkMeta = static_cast<SuperchunkMeta*>(std::malloc(sizeof(SuperchunkMeta)      * nSuperchunks));
	s_drawCmds       = static_cast<TerrainDrawCommand*>(std::malloc(sizeof(TerrainDrawCommand)  * nBlocks));
	s_skirtDepths    = std::make_unique<float[]>(nBlocks);

	gosASSERT(s_blockMeta      != nullptr);
	gosASSERT(s_superchunkMeta != nullptr);
	gosASSERT(s_drawCmds       != nullptr);
	gosASSERT(s_skirtDepths    != nullptr);

	memset(s_blockMeta,      0, sizeof(TerrainBlockMeta)   * nBlocks);
	memset(s_superchunkMeta, 0, sizeof(SuperchunkMeta)      * nSuperchunks);
	memset(s_drawCmds,       0, sizeof(TerrainDrawCommand)  * nBlocks);
	memset(s_skirtDepths.get(),    0, sizeof(float)               * nBlocks);

	gCurrentFrame = 1;
	s_cmdCount    = 0;

	// Phase 7.5 diagnostic: startup banner — unmistakable confirmation the
	// chunk renderer is active on this launch.
	mc2::fmt::print_out("[TerrainLOD v1] ENABLED chunks={:d} x={:d} ssbo=binding23 drawPath=chunk\n",
	       mc2::fmt::as_i32(s_terrainChunkSide * s_terrainChunkSide), mc2::fmt::as_i32(s_terrainChunkSide));
	fflush(stdout);

	// Initialize per-block metadata.
	for (int by = 0; by < s_terrainChunkSide; ++by) {
		for (int bx = 0; bx < s_terrainChunkSide; ++bx) {
			TerrainBlockMeta& bm = s_blockMeta[bx + by * s_terrainChunkSide];
			bm.originX    = bx * static_cast<int>(verticesBlockSide);
			bm.originY    = by * static_cast<int>(verticesBlockSide);
			int qx = static_cast<int>((realVerticesMapSide - 1)) - bm.originX;
			int qy = static_cast<int>((realVerticesMapSide - 1)) - bm.originY;
			bm.quadCountX = (qx < static_cast<int>(verticesBlockSide)) ? qx : static_cast<int>(verticesBlockSide);
			bm.quadCountY = (qy < static_cast<int>(verticesBlockSide)) ? qy : static_cast<int>(verticesBlockSide);
			bm.dirtyAabb  = false;
			bm.inFrustum  = false;
			bm.lodLevel   = 0;
			if (mapData && mapData->getBlocks())
				recomputeBlockAabb(bm);
		}
	}

	// Initialize superchunk AABBs.
	for (int scY = 0; scY < s_superchunkSide; ++scY)
		for (int scX = 0; scX < s_superchunkSide; ++scX)
			recomputeSuperchunkAabb(scX, scY);
}

// LOD chunk: upload the full heightfield and per-vertex material to the GPU.
void Terrain::lodHeightUpload(TerrainLodInitFrame& lf) {
	auto& elev = lf.elev;
	auto& ttype = lf.ttype;
	int n = static_cast<int>(realVerticesMapSide) * static_cast<int>(realVerticesMapSide);
	elev.assign(static_cast<size_t>(n), 0.0f);
	ttype.assign(static_cast<size_t>(n), 0.0f);  // Step 5b: per-vertex MATERIAL index (0-3)
	const PostcompVertex* blks = mapData->getBlocks();
	// Map the raw terrainType INDEX -> material (0=Rock,1=Grass,2=Dirt,
	// 3=Concrete). MUST mirror gos_terrain_indirect.cpp
	// terrainTypeToMaterialLocal / quad.cpp terrainTypeToMaterial. The chunk
	// frag does smoothstep(2,3,material) for concrete, so uploading the raw
	// index (0-20) would mark most terrain as concrete.
	auto terrainTypeToMaterial = [](DWORD t) -> float { return static_cast<float>(mc2terrain::terrainTypeToMaterial(static_cast<uint32_t>(t))); };
	for (int i = 0; i < n; ++i)
	{
		elev[i]  = blks[i].elevation;
		ttype[i] = terrainTypeToMaterial(blks[i].terrainType);
	}
	gos_TerrainLodChunk_UploadHeightFull(elev.data(), static_cast<int>(realVerticesMapSide));
	gos_TerrainLodChunk_UploadTerrainTypeFull(ttype.data(), static_cast<int>(realVerticesMapSide));
}

// LOD chunk: load the control-map sidecar when its gate is on.
void Terrain::lodControlMap() {
	// TERRAIN-CONTROLMAP-SAMPLE-1: v1 = OVERRIDE-ONLY PASSTHROUGH. Gate
	// MC2_TERRAIN_CONTROLMAP default OFF (mirrors MC2_TERRAIN_LOD_CHUNK's
	// read pattern above). When ON, load an authored sidecar PNG (RGBA8,
	// vertex-resolution side*side) if present; when absent or gate OFF,
	// no texture is created and u_useControlMap uploads 0 at draw ->
	// byte-identical to the legacy chunkColorWeights() classifier path.
	// MC2_TERRAIN_CONTROLMAP_FILE overrides the sidecar path (precedent:
	// MC2_TERRAIN_VISUAL_HEIGHT_FILE above).
	{
		static const bool s_controlMapGate = mc2::gate::get<"MC2_TERRAIN_CONTROLMAP">();
		const int mapSide = static_cast<int>(realVerticesMapSide);
		if (s_controlMapGate && mapSide > 1)
		{
			char cmPath[600];
			if (mc2::gate::isSet<"MC2_TERRAIN_CONTROLMAP_FILE">())   // set, even when empty (the old getenv test)
			{
				const std::string ovStr = mc2::gate::str<"MC2_TERRAIN_CONTROLMAP_FILE">();
				strncpy(cmPath, ovStr.c_str(), sizeof(cmPath) - 1);
				cmPath[sizeof(cmPath) - 1] = '\0';
			}
			else
			{
				const char* nm = terrainName ? terrainName : "";
				const std::string stemStr = std::filesystem::path(nm).stem().string();
				const char* stem = stemStr.c_str();
				mc2::fmt::to_buf(cmPath, "data/missions/{}.beauty/control_map.png", mc2::fmt::safe(stem));
			}
			FILE* cf = fopen(cmPath, "rb");
			if (!cf)
			{
				mc2::fmt::print_out("[TERRAIN_CONTROLMAP v1] sidecar NOT FOUND path={} (gate on, mapSide={:d}) -- passthrough (legacy classifier)\n", mc2::fmt::safe(cmPath), mc2::fmt::as_i32(mapSide));
				fflush(stdout);
			}
			else
			{
				fseek(cf, 0, SEEK_END);
				long cFileSize = ftell(cf);
				fseek(cf, 0, SEEK_SET);
				if (cFileSize <= 0)
				{
					fclose(cf);
					mc2::fmt::print_out("[TERRAIN_CONTROLMAP v1] EMPTY FILE path={}\n", mc2::fmt::safe(cmPath));
					fflush(stdout);
				}
				else
				{
					std::vector<unsigned char> pngBytes(static_cast<size_t>(cFileSize));
					size_t rd = fread(pngBytes.data(), 1, static_cast<size_t>(cFileSize), cf);
					fclose(cf);
					if (rd != static_cast<size_t>(cFileSize))
					{
						mc2::fmt::print_out("[TERRAIN_CONTROLMAP v1] READ FAIL path={} read={:d} want={:d}\n", mc2::fmt::safe(cmPath), mc2::fmt::as_size(rd), mc2::fmt::as_i32(cFileSize));
						fflush(stdout);
					}
					else
					{
						int pw = 0, ph = 0;
						unsigned char* rgba = ControlMapPng_DecodeRGBA(
							pngBytes.data(), static_cast<int>(cFileSize), &pw, &ph);
						if (!rgba)
						{
							mc2::fmt::print_out("[TERRAIN_CONTROLMAP v1] PNG DECODE FAILED path={}\n", mc2::fmt::safe(cmPath));
							fflush(stdout);
						}
						else if (pw != mapSide || ph != mapSide)
						{
							mc2::fmt::print_out("[TERRAIN_CONTROLMAP v1] SIZE MISMATCH path={} got={:d}x{:d} want={:d}x{:d} -- passthrough (legacy classifier)\n", mc2::fmt::safe(cmPath), mc2::fmt::as_i32(pw), mc2::fmt::as_i32(ph), mc2::fmt::as_i32(mapSide), mc2::fmt::as_i32(mapSide));
							fflush(stdout);
							ControlMapPng_FreePixels(rgba);
						}
						else
						{
							gos_TerrainLodChunk_UploadControlMap(rgba, mapSide);
							mc2::fmt::print_out("[TERRAIN_CONTROLMAP v1] LOADED path={} side={:d}\n", mc2::fmt::safe(cmPath), mc2::fmt::as_i32(mapSide));
							fflush(stdout);
							ControlMapPng_FreePixels(rgba);
						}
					}
				}
			}
		}
	}
}

// LOD chunk: load the overlay-v2 sidecar when its gate is on.
void Terrain::lodOverlayV2() {
	// TERRAIN-OVERLAY-V2-PARITY-1: v1 = ADDITIVE sidecar composite (legacy
	// cement-word + overlay pass stays verbatim). Gate MC2_TERRAIN_OVERLAY_V2
	// default OFF (mirrors MC2_TERRAIN_CONTROLMAP's read pattern above). When
	// ON, load an authored bounds-aware overlay sidecar PNG (RGBA8, arbitrary
	// WxH -- NOT vertex-resolution) if present; when absent or gate OFF, no
	// texture is created and u_useOverlaySidecar uploads 0 at draw ->
	// byte-identical to the legacy cement-word + separate overlay-pass
	// composite. Sidecar carries world bounds via a companion
	// "<name>.bounds.txt" (4 whitespace-separated floats: topLeftX topLeftY
	// sizeX sizeY, world units; topLeftX=MIN world X, topLeftY=MAX world Y
	// -- same convention as the colormap atlas uniforms, PNG row 0 = north
	// edge, no flip) -- absent bounds file falls back to the full-map extent.
	// MC2_TERRAIN_OVERLAY_V2_FILE overrides the sidecar path (precedent:
	// MC2_TERRAIN_CONTROLMAP_FILE above).
	{
		static const bool s_overlayV2Gate = mc2::gate::get<"MC2_TERRAIN_OVERLAY_V2">();
		if (s_overlayV2Gate)
		{
			char ovPath[600];
			if (mc2::gate::isSet<"MC2_TERRAIN_OVERLAY_V2_FILE">())   // set, even when empty (the old getenv test)
			{
				const std::string ovStr = mc2::gate::str<"MC2_TERRAIN_OVERLAY_V2_FILE">();
				strncpy(ovPath, ovStr.c_str(), sizeof(ovPath) - 1);
				ovPath[sizeof(ovPath) - 1] = '\0';
			}
			else
			{
				const char* nm = terrainName ? terrainName : "";
				const std::string stemStr = std::filesystem::path(nm).stem().string();
				const char* stem = stemStr.c_str();
				mc2::fmt::to_buf(ovPath, "data/missions/{}.beauty/overlay_v2.png", mc2::fmt::safe(stem));
			}
			FILE* of = fopen(ovPath, "rb");
			if (!of)
			{
				mc2::fmt::print_out("[TERRAIN_OVERLAY_V2 v1] sidecar NOT FOUND path={} (gate on) -- passthrough (legacy cement-word + overlay pass)\n", mc2::fmt::safe(ovPath));
				fflush(stdout);
			}
			else
			{
				fseek(of, 0, SEEK_END);
				long oFileSize = ftell(of);
				fseek(of, 0, SEEK_SET);
				if (oFileSize <= 0)
				{
					fclose(of);
					mc2::fmt::print_out("[TERRAIN_OVERLAY_V2 v1] EMPTY FILE path={}\n", mc2::fmt::safe(ovPath));
					fflush(stdout);
				}
				else
				{
					std::vector<unsigned char> pngBytes(static_cast<size_t>(oFileSize));
					size_t rd = fread(pngBytes.data(), 1, static_cast<size_t>(oFileSize), of);
					fclose(of);
					if (rd != static_cast<size_t>(oFileSize))
					{
						mc2::fmt::print_out("[TERRAIN_OVERLAY_V2 v1] READ FAIL path={} read={:d} want={:d}\n", mc2::fmt::safe(ovPath), mc2::fmt::as_size(rd), mc2::fmt::as_i32(oFileSize));
						fflush(stdout);
					}
					else
					{
						int ow = 0, oh = 0;
						unsigned char* orgba = ControlMapPng_DecodeRGBA(
							pngBytes.data(), static_cast<int>(oFileSize), &ow, &oh);
						if (!orgba)
						{
							mc2::fmt::print_out("[TERRAIN_OVERLAY_V2 v1] PNG DECODE FAILED path={}\n", mc2::fmt::safe(ovPath));
							fflush(stdout);
						}
						else
						{
							// World bounds: default = full map extent, using the
							// SAME (topLeftX, topLeftY=maxY) convention as the
							// colormap atlas uniforms (u_atlasTopLeftX/Y) so PNG
							// row 0 (top) == north edge == vertex row 0, no flip
							// (matches control_map_tool.py's documented convention).
							// Companion bounds file overrides for a tighter
							// authored region: "topLeftX topLeftY sizeX sizeY".
							float halfMapOv = Terrain::worldUnitsMapSide * 0.5f;
							float bTLX = -halfMapOv, bTLY = halfMapOv;
							float bSizeX = 2.0f * halfMapOv, bSizeY = 2.0f * halfMapOv;
							char boundsPath[620];
							mc2::fmt::to_buf(boundsPath, "{}", mc2::fmt::safe(ovPath));
							size_t plen = strlen(boundsPath);
							if (plen > 4 && strcmp(boundsPath + plen - 4, ".png") == 0)
								boundsPath[plen - 4] = '\0';
							strncat(boundsPath, ".bounds.txt", sizeof(boundsPath) - strlen(boundsPath) - 1);
							FILE* bf = fopen(boundsPath, "r");
							if (bf)
							{
								float bx, by, bw, bh;
								if (fscanf(bf, "%f %f %f %f", &bx, &by, &bw, &bh) == 4)
								{
									bTLX = bx; bTLY = by; bSizeX = bw; bSizeY = bh;
									mc2::fmt::print_out("[TERRAIN_OVERLAY_V2 v1] bounds override path={} ({:.1f},{:.1f},{:.1f},{:.1f})\n", mc2::fmt::safe(boundsPath), bTLX, bTLY, bSizeX, bSizeY);
									fflush(stdout);
								}
								fclose(bf);
							}
							gos_TerrainLodChunk_UploadOverlaySidecar(orgba, ow, oh,
							                                          bTLX, bTLY, bSizeX, bSizeY);
							mc2::fmt::print_out("[TERRAIN_OVERLAY_V2 v1] LOADED path={} size={:d}x{:d} bounds=({:.1f},{:.1f},{:.1f},{:.1f})\n", mc2::fmt::safe(ovPath), mc2::fmt::as_i32(ow), mc2::fmt::as_i32(oh), bTLX, bTLY, bSizeX, bSizeY);
							fflush(stdout);
							ControlMapPng_FreePixels(orgba);
						}
					}
				}
			}
		}
	}
}

// LOD chunk: load the shoreline-mask sidecar when its gate is on.
void Terrain::lodShorelineMask() {
	if (s_beautySig.state == 2) {
		mc2::fmt::print_out("[SHORELINE v1] REJECTED: gameplay height signature mismatch; shoreline_mask sidecar not loaded\n");
		return;
	}
	// TERRAIN-SHORELINE-V3: terrain-side wet/foam band. The gate is
	// default ON (SHORELINE-SMOOTHING-REVISIT-1); this comment claimed
	// OFF until SHORELINE-GLOW-1. As of V3 the band's PLACEMENT is computed in
	// the frag from ELEVATION (v_worldPos.z vs u_waterElevation) and
	// needs no sidecar at all -- gate ON alone is enough to show
	// full elevation bands. This load block is now OPTIONAL: when
	// present, an authored bounds-aware shoreline mask PNG (RGBA8,
	// arbitrary WxH -- offline-cooked by
	// tools/terrain_beautify/cook_shoreline.py) is loaded and applied
	// as a MODULATOR (wide-beach falloff / basin exclusion) on top of
	// the elevation bands; when absent, no texture is created and
	// u_hasShorelineMask uploads 0 at draw -> pure elevation bands (no
	// modulation, not "no band"). Gate OFF -> u_useShorelineMask
	// uploads 0 -> byte-identical (no wet/foam band; legacy screen
	// runShoreline() stays active). Companion "<name>.bounds.txt" --
	// SAME convention as overlay_v2.bounds.txt (topLeftX topLeftY
	// sizeX sizeY, world units; absent -> full map extent).
	// MC2_TERRAIN_SHORELINE_FILE overrides the sidecar path
	// (precedent: MC2_TERRAIN_OVERLAY_V2_FILE above).
	{
		// SHORELINE-GLOW-1: was a THIRD independent read of the same
		// variable, whose comment asserted it "matches the two reads in
		// gos_terrain_lod_chunk.cpp" -- and one of those two did not
		// match. Routed through the single shared accessor so the claim
		// is now true by construction instead of by inspection.
		const bool s_shorelineGate = gos_TerrainShorelineGateOn();
		// WATER-SHORE-EDGE-1 (decouple): the WATER shader's mask feather
		// (modes 1/2 fallback) consumes this same sidecar, so load it
		// whenever the water shore mode wants it, independent of the
		// TERRAIN band toggle (MC2_TERRAIN_SHORELINE). Previously
		// MC2_TERRAIN_SHORELINE=0 silently killed the water feather too.
		if (s_shorelineGate || gos_WaterShoreMode() >= 1)
		{
			char slPath[600];
			if (mc2::gate::isSet<"MC2_TERRAIN_SHORELINE_FILE">())   // set, even when empty (the old getenv test)
			{
				const std::string ovStr = mc2::gate::str<"MC2_TERRAIN_SHORELINE_FILE">();
				strncpy(slPath, ovStr.c_str(), sizeof(slPath) - 1);
				slPath[sizeof(slPath) - 1] = '\0';
			}
			else
			{
				const char* nm = terrainName ? terrainName : "";
				const std::string stemStr = std::filesystem::path(nm).stem().string();
				const char* stem = stemStr.c_str();
				mc2::fmt::to_buf(slPath, "data/missions/{}.beauty/shoreline_mask.png", mc2::fmt::safe(stem));
			}
			FILE* sf = fopen(slPath, "rb");
			if (!sf)
			{
				// FAIL-LOUD (DEPLOY-CURRENT-1): this was MC2_DIAGF, which only
				// prints under MC2_DIAG_VERBOSE. MC2_TERRAIN_SHORELINE is
				// default-ON, so on a STOCK launch a missing sidecar produced
				// no output at all and the gate silently no-opped -- the four
				// sibling loaders in this file (CONTROLMAP / OVERLAY_V2 /
				// VISUAL_HEIGHT / VISUAL_DAMP) all use raw printf, and every
				// one of them is default-OFF. The only loader a default launch
				// actually reaches was the only silent one. Raw printf now, so
				// "gate on but doing nothing" is visible without opting in.
				mc2::fmt::print_out("[TERRAIN_SHORELINE v1] sidecar NOT FOUND path={} (gate on) -- pure elevation bands, no mask modulator\n", mc2::fmt::safe(slPath));
				fflush(stdout);
			}
			else
			{
				fseek(sf, 0, SEEK_END);
				long sFileSize = ftell(sf);
				fseek(sf, 0, SEEK_SET);
				if (sFileSize <= 0)
				{
					fclose(sf);
					mc2::fmt::print_out("[TERRAIN_SHORELINE v1] EMPTY FILE path={}\n", mc2::fmt::safe(slPath));
					fflush(stdout);
				}
				else
				{
					std::vector<unsigned char> pngBytes(static_cast<size_t>(sFileSize));
					size_t rd = fread(pngBytes.data(), 1, static_cast<size_t>(sFileSize), sf);
					fclose(sf);
					if (rd != static_cast<size_t>(sFileSize))
					{
						mc2::fmt::print_out("[TERRAIN_SHORELINE v1] READ FAIL path={} read={:d} want={:d}\n", mc2::fmt::safe(slPath), mc2::fmt::as_size(rd), mc2::fmt::as_i32(sFileSize));
						fflush(stdout);
					}
					else
					{
						int sw = 0, sh = 0;
						unsigned char* srgba = ControlMapPng_DecodeRGBA(
							pngBytes.data(), static_cast<int>(sFileSize), &sw, &sh);
						if (!srgba)
						{
							mc2::fmt::print_out("[TERRAIN_SHORELINE v1] PNG DECODE FAILED path={}\n", mc2::fmt::safe(slPath));
							fflush(stdout);
						}
						else
						{
							// World bounds: default = full map extent, SAME
							// (topLeftX, topLeftY=maxY) convention as
							// overlay-V2. Companion bounds file overrides.
							float halfMapSl = Terrain::worldUnitsMapSide * 0.5f;
							float sTLX = -halfMapSl, sTLY = halfMapSl;
							float sSizeX = 2.0f * halfMapSl, sSizeY = 2.0f * halfMapSl;
							char sBoundsPath[620];
							mc2::fmt::to_buf(sBoundsPath, "{}", mc2::fmt::safe(slPath));
							size_t splen = strlen(sBoundsPath);
							if (splen > 4 && strcmp(sBoundsPath + splen - 4, ".png") == 0)
								sBoundsPath[splen - 4] = '\0';
							strncat(sBoundsPath, ".bounds.txt", sizeof(sBoundsPath) - strlen(sBoundsPath) - 1);
							FILE* sbf = fopen(sBoundsPath, "r");
							if (sbf)
							{
								float sbx, sby, sbw, sbh;
								if (fscanf(sbf, "%f %f %f %f", &sbx, &sby, &sbw, &sbh) == 4)
								{
									sTLX = sbx; sTLY = sby; sSizeX = sbw; sSizeY = sbh;
									MC2_DIAGF("[TERRAIN_SHORELINE v1] bounds override path=%s (%.1f,%.1f,%.1f,%.1f)\n",
									       sBoundsPath, sTLX, sTLY, sSizeX, sSizeY);
									fflush(stdout);
								}
								fclose(sbf);
							}
							gos_TerrainLodChunk_UploadShorelineMask(srgba, sw, sh,
							                                         sTLX, sTLY, sSizeX, sSizeY);
							MC2_DIAGF("[TERRAIN_SHORELINE v1] LOADED path=%s size=%dx%d bounds=(%.1f,%.1f,%.1f,%.1f)\n",
							       slPath, sw, sh, sTLX, sTLY, sSizeX, sSizeY);
							fflush(stdout);
							ControlMapPng_FreePixels(srgba);
						}
					}
				}
			}
		}
	}
}

namespace {
// SHORE-FIELD-1: IEEE half -> float for the float16 shore field sidecar.
float shoreFieldHalfToFloat(uint16_t h) {
	const uint32_t sign = static_cast<uint32_t>((h & 0x8000u)) << 16;
	uint32_t exp = (h >> 10) & 0x1Fu;
	uint32_t man = h & 0x3FFu;
	uint32_t bits;
	if (exp == 0) {
		if (man == 0) bits = sign;
		else {   // subnormal: renormalise
			exp = 1;
			while ((man & 0x400u) == 0) { man <<= 1; --exp; }
			man &= 0x3FFu;
			bits = sign | ((exp + 112u) << 23) | (man << 13);
		}
	} else if (exp == 31) bits = sign | 0x7F800000u | (man << 13);
	else bits = sign | ((exp + 112u) << 23) | (man << 13);
	float f;
	std::memcpy(&f, &bits, sizeof(f));
	return f;
}

// SHORE-FIELD-1: the cooked coastline field (tools/terrain_beautify/cook_shore_field.py):
// a float16 pseudo-height grid stored relative to the water level, corner-aligned on the map (side = (mapSide-1)*k+1 for any
// k >= 1). Returns false when the sidecar is absent or does not fit the map.
bool loadShoreField(mc2::czstring stem, int mapSide, std::vector<float>& out, int& side) {
	char path[600];
	mc2::fmt::to_buf(path, "data/missions/{}.beauty/shore_field.r16f", mc2::fmt::safe(stem));
	FILE* f = fopen(path, "rb");  // path-audit: loose-only
	if (!f) return false;
	// Header: "SHF1", uint32 side, float32 water elevation, uint32 reserved; then side*side float16
	// of (height - water elevation) -- float16 is precise near zero, where the coast is cut.
	fseek(f, 0, SEEK_END);
	const long bytes = ftell(f);
	fseek(f, 0, SEEK_SET);
	char magic[4] = {};
	uint32_t hdrSide = 0, hdrPad = 0;
	float hdrWater = 0.0f;
	const bool hdrOk = fread(magic, 1, 4, f) == 4 && std::memcmp(magic, "SHF1", 4) == 0 &&
	                   fread(&hdrSide, 4, 1, f) == 1 && fread(&hdrWater, 4, 1, f) == 1 && fread(&hdrPad, 4, 1, f) == 1;
	const int s = static_cast<int>(hdrSide);
	const uint64_t expectedBytes = 16ull + 2ull * static_cast<uint64_t>(hdrSide) * static_cast<uint64_t>(hdrSide);
	if (!hdrOk || !std::isfinite(hdrWater) || s < 2 || expectedBytes != static_cast<uint64_t>(bytes) ||
	    mapSide < 2 || (s - 1) % (mapSide - 1) != 0) {
		mc2::fmt::print_out("[WATER_SHORE_EDGE v1] shore field {} does not fit the map (bytes={:d} mapSide={:d}) -- ignored\n",
		                    mc2::fmt::safe(path), mc2::fmt::as_i32(bytes), mc2::fmt::as_i32(mapSide));
		fclose(f);
		return false;
	}
	std::vector<uint16_t> halves(static_cast<size_t>(s) * static_cast<size_t>(s));
	const size_t rd = fread(halves.data(), sizeof(uint16_t), halves.size(), f);
	fclose(f);
	if (rd != halves.size()) return false;
	out.resize(halves.size());
	for (size_t i = 0; i < halves.size(); ++i) {
		out[i] = shoreFieldHalfToFloat(halves[i]) + hdrWater;
		if (!std::isfinite(out[i])) {
			mc2::fmt::print_out("[WATER_SHORE_EDGE v1] shore field {} contains a non-finite value at texel {:d} -- ignored\n",
			                    mc2::fmt::safe(path), mc2::fmt::as_size(i));
			return false;
		}
	}
	side = s;
	MC2_DIAGF("[WATER_SHORE_EDGE v1] LOADED shore field path=%s side=%d\n", path, s);
	return true;
}
}  // namespace

// LOD chunk: build the water-shore edge data when the shore mode asks for it.
void Terrain::lodWaterShore() {
	if (s_beautySig.state == 2) {
		mc2::fmt::print_out("[WATER_SHORE_EDGE v1] REJECTED: gameplay height signature mismatch; shore_field and visual_height reads skipped\n");
		return;
	}
	// WATER-SHORE-EDGE-1: per-fragment water shore edge (mode 2, the
	// default -- gos_WaterShoreMode()). Loads the SAME hi-res
	// visual_height_4x.r32 sidecar the chunk-mesh displacement uses
	// (V x V floats, V=(mapSide-1)*4+1) and uploads it as an R32F
	// GL_LINEAR texture so the water FS computes a continuous signed
	// crossing (u_waterElevation - h) instead of the per-vertex coarse
	// WaterThickness ramp (whose iso-line degenerates onto the 128wu
	// tile lattice) or the ~8wu-stepped cooked mask contour. Sidecar
	// absent -> no upload -> draw path falls back to mode-1 (mask)
	// behaviour, unchanged pixels. Independent of both
	// MC2_TERRAIN_SHORELINE and MC2_TERRAIN_VISUAL_HEIGHT.
	{
		const int wsMapSide = static_cast<int>(realVerticesMapSide);
		if (gos_WaterShoreMode() >= 2 && wsMapSide > 1)
		{
			const int WV = (wsMapSide - 1) * 4 + 1;
			char wsPath[600];
			const float wsHalfF = Terrain::worldUnitsMapSide * 0.5f;
			const float wsSpanF = static_cast<float>((wsMapSide - 1)) * Terrain::worldUnitsPerVertex;
			if (mc2::gate::isSet<"MC2_TERRAIN_VISUAL_HEIGHT_FILE">())   // set, even when empty (the old getenv test)
			{
				const std::string ovStr = mc2::gate::str<"MC2_TERRAIN_VISUAL_HEIGHT_FILE">();
				strncpy(wsPath, ovStr.c_str(), sizeof(wsPath) - 1);
				wsPath[sizeof(wsPath) - 1] = '\0';
			}
			else
			{
				const char* nm = terrainName ? terrainName : "";
				const std::string stemStr = std::filesystem::path(nm).stem().string();
				const char* stem = stemStr.c_str();
				// SHORE-FIELD-1: the cooked smooth coastline field wins over the raw visual heights
				// (same bounds contract, any corner-aligned density).
				std::vector<float> sf;
				int sfSide = 0;
				if (loadShoreField(stem, wsMapSide, sf, sfSide)) {
					gos_TerrainLodChunk_UploadWaterShoreSdf(sf.data(), sfSide, sfSide, -wsHalfF, wsHalfF, wsSpanF, wsSpanF);
					return;
				}
				mc2::fmt::to_buf(wsPath, "data/missions/{}.beauty/visual_height_4x.r32", mc2::fmt::safe(stem));
			}
			FILE* wf = fopen(wsPath, "rb");  // path-audit: loose-only
			if (!wf)
			{
				mc2::fmt::print_out("[WATER_SHORE_EDGE v1] hi-res height NOT FOUND path={} (mode 2 requested) -- water shore falls back to mask mode\n", mc2::fmt::safe(wsPath));
				fflush(stdout);
			}
			else
			{
				fseek(wf, 0, SEEK_END);
				long wsz = ftell(wf);
				fseek(wf, 0, SEEK_SET);
				const size_t wsWant = static_cast<size_t>(WV) * static_cast<size_t>(WV) * sizeof(float);
				if (static_cast<size_t>(wsz) != wsWant)
				{
					mc2::fmt::print_out("[WATER_SHORE_EDGE v1] SIZE MISMATCH path={} got={:d} want={:d} (V={:d} mapSide={:d}) -- ignored\n", mc2::fmt::safe(wsPath), mc2::fmt::as_i32(wsz), mc2::fmt::as_size(wsWant), mc2::fmt::as_i32(WV), mc2::fmt::as_i32(wsMapSide));
					fflush(stdout);
					fclose(wf);
				}
				else
				{
					std::vector<float> wsh(static_cast<size_t>(WV) * static_cast<size_t>(WV));
					size_t rd = fread(wsh.data(), sizeof(float), wsh.size(), wf);
					fclose(wf);
					if (rd != wsh.size())
					{
						mc2::fmt::print_out("[WATER_SHORE_EDGE v1] READ FAIL path={} read={:d} want={:d}\n", mc2::fmt::safe(wsPath), mc2::fmt::as_size(rd), mc2::fmt::as_size(wsh.size()));
						fflush(stdout);
					}
					else
					{
						// Bounds contract (see gos_terrain_lod_chunk.h):
						// topLeft = world position of fine vertex (0,0)
						// = (-halfMap, +halfMap); size = GRID SPAN
						// = (V-1) * (worldUnitsPerVertex/4)
						// = (mapSide-1) * worldUnitsPerVertex. Shader
						// half-texel remap puts texel centres on verts.
						const float wsHalf = Terrain::worldUnitsMapSide * 0.5f;
						const float wsSpan = static_cast<float>((wsMapSide - 1)) * Terrain::worldUnitsPerVertex;
						gos_TerrainLodChunk_UploadWaterShoreHeight(
							wsh.data(), WV, WV, -wsHalf, wsHalf, wsSpan, wsSpan);
						MC2_DIAGF("[WATER_SHORE_EDGE v1] LOADED path=%s V=%d span=%.1fwu\n",
						          wsPath, WV, wsSpan);
					}
				}
			}
		}
	}
}

// LOD chunk: load the 4x visual heightfield bake when its gate is on.
void Terrain::lodVisualHeight(TerrainLodInitFrame& lf) {
	auto& elev = lf.elev;
	TerrainRuntime::setCpuVisualHeight(nullptr, 0);
	gos_TerrainLodChunk_ClearVisualHeight();   // a rejected or missing bake must not draw the previous mission's relief
	// TERRAIN-VISUAL-HEIGHT-SAMPLE-1 Stage 1 (loader, log-only). Gate
	// MC2_TERRAIN_VISUAL_HEIGHT default-OFF -> no load, no SSBO
	// (byte-identical). Loads the 4x VISUAL heightfield bake into SSBO
	// binding 26 for future corner-pinned interior displacement (Stage 2).
	// Path: MC2_TERRAIN_VISUAL_HEIGHT_FILE override, else
	// data/missions/<mission-stem>.beauty/visual_height_4x.r32. NO geometry
	// consumes binding 26 yet, so this is render-invariant.
	{
		// displace implies load
		static const bool s_visualHeightGate = mc2::gate::get<"MC2_TERRAIN_VISUAL_HEIGHT">() ||
		                                       mc2::gate::get<"MC2_TERRAIN_VISUAL_DISPLACE">();
		const int mapSide = static_cast<int>(realVerticesMapSide);
		if (s_visualHeightGate && mapSide > 1)
		{
			const int V = (mapSide - 1) * 4 + 1;
			char vhPath[600];
			if (mc2::gate::isSet<"MC2_TERRAIN_VISUAL_HEIGHT_FILE">())   // set, even when empty (the old getenv test)
			{
				const std::string ovStr = mc2::gate::str<"MC2_TERRAIN_VISUAL_HEIGHT_FILE">();
				strncpy(vhPath, ovStr.c_str(), sizeof(vhPath) - 1);
				vhPath[sizeof(vhPath) - 1] = '\0';
			}
			else
			{
				const char* nm = terrainName ? terrainName : "";
				const std::string stemStr = std::filesystem::path(nm).stem().string();
				const char* stem = stemStr.c_str();
				mc2::fmt::to_buf(vhPath, "data/missions/{}.beauty/visual_height_4x.r32", mc2::fmt::safe(stem));
			}
			if (s_beautySig.state == 2)   // one verdict gates the bake: no read, no damp, no mover stamp, no LOADED line
				{
					mc2::fmt::print_out("[VISUAL_HEIGHT v1] REJECTED path={} gameplay height signature mismatch (sidecar '{}' vs loaded '{}'): bake does not belong to this terrain (mod?); drawing undisplaced\n", mc2::fmt::safe(vhPath), mc2::fmt::safe(s_beautySig.got), mc2::fmt::safe(s_beautySig.want));
					fflush(stdout);
					return;
				}
				FILE* vf = fopen(vhPath, "rb");
			if (!vf)
			{
				mc2::fmt::print_out("[VISUAL_HEIGHT v1] bake NOT FOUND path={} (gate on, V={:d} mapSide={:d})\n", mc2::fmt::safe(vhPath), mc2::fmt::as_i32(V), mc2::fmt::as_i32(mapSide));
				fflush(stdout);
			}
			else
			{
				fseek(vf, 0, SEEK_END);
				long vsz = ftell(vf);
				fseek(vf, 0, SEEK_SET);
				const size_t want = static_cast<size_t>(V) * static_cast<size_t>(V) * sizeof(float);
				if (static_cast<size_t>(vsz) != want)
				{
					mc2::fmt::print_out("[VISUAL_HEIGHT v1] SIZE MISMATCH path={} got={:d} want={:d} (V={:d} mapSide={:d})\n", mc2::fmt::safe(vhPath), mc2::fmt::as_i32(vsz), mc2::fmt::as_size(want), mc2::fmt::as_i32(V), mc2::fmt::as_i32(mapSide));
					fflush(stdout);
					fclose(vf);
				}
				else
				{
					std::vector<float> vh(static_cast<size_t>(V) * static_cast<size_t>(V));
					size_t rd = fread(vh.data(), sizeof(float), vh.size(), vf);
					fclose(vf);
					if (rd != vh.size())
					{
						mc2::fmt::print_out("[VISUAL_HEIGHT v1] READ FAIL path={} read={:d} want={:d}\n", mc2::fmt::safe(vhPath), mc2::fmt::as_size(rd), mc2::fmt::as_size(vh.size()));
						fflush(stdout);
					}
					else
					{
						// TERRAIN-LOD-GEOMORPH-1: optional max-mip sidecar
						// (visual_height_mips.r32, sibling of the fine bake):
						// 5 levels (strides 2,4,5,10,20) x mapSide^2 floats,
						// max of the fine bake over each coarse vertex's
						// +/- stride/2 footprint. Appended to the binding-26
						// SSBO by the upload below. Absent/mis-sized -> no
						// mips (legacy layout; coarse bands behave as S2).
						std::vector<float> vmips;
						{
							const std::filesystem::path mipFsPath = std::filesystem::path(vhPath).replace_filename("visual_height_mips.r32");
							const std::string mipPathStr = mipFsPath.string();
							const char* mipPath = mipPathStr.c_str();
							if (mipFsPath.has_parent_path())
							{
								FILE* mf = fopen(mipPath, "rb");
								if (mf)
								{
									fseek(mf, 0, SEEK_END);
									long msz = ftell(mf);
									fseek(mf, 0, SEEK_SET);
									const size_t mipWant =
										5u * static_cast<size_t>(mapSide) * static_cast<size_t>(mapSide) * sizeof(float);
									if (static_cast<size_t>(msz) == mipWant)
									{
										vmips.resize(mipWant / sizeof(float));
										if (fread(vmips.data(), sizeof(float), vmips.size(), mf)
										    != vmips.size())
											vmips.clear();
									}
									else
									{
										mc2::fmt::print_out("[VISUAL_HEIGHT v1] mips SIZE MISMATCH path={} got={:d} want={:d} (5 x {:d}^2 floats) -- ignored\n", mc2::fmt::safe(mipPath), mc2::fmt::as_i32(msz), mc2::fmt::as_size(mipWant), mc2::fmt::as_i32(mapSide));
										fflush(stdout);
									}
									// TERRAIN-LOD-GEOMORPH-1 STALENESS GUARD
									// (GEOMORPH-NEXT-SLICES.md coordination ruling):
									// the mips sidecar is regenerated SEPARATELY
									// from the fine bake, so a stale
									// visual_height_mips.r32 that still matches
									// mapSide (right SIZE, wrong DATA) was silently
									// accepted -> coarse bands morph to a surface
									// that no longer exists. Catch it with a
									// zero-cost data invariant (no hash / new file):
									// every mip level is a MAX over a footprint that
									// INCLUDES the coarse vertex's own fine sample,
									// so mip[L][v] >= fineCorner[v] must hold for a
									// consistent bake; a stale bake violates it. On
									// any violation DROP the mips and fall back to
									// the known-safe legacy S2 layout (+ loud warn)
									// rather than morph to a dead surface. Fresh
									// bakes are unaffected (default path stays
									// byte-identical).
									if (!vmips.empty())
									{
										const int mipFactor = (mapSide > 1)
											? (V - 1) / (mapSide - 1) : 1;
										const float kFloorTol = 0.05f; // wu; f32 max round headroom
										const size_t plane =
											static_cast<size_t>(mapSide) * static_cast<size_t>(mapSide);
										const size_t nLevels =
											plane ? (vmips.size() / plane) : 0;
										size_t viol = 0;
										float worstDeficit = 0.0f;
										for (int r = 0; r < mapSide && viol == 0; ++r)
										{
											for (int c = 0; c < mapSide; ++c)
											{
												const float corner = vh[
													static_cast<size_t>((r * mipFactor)) * static_cast<size_t>(V)
													+ static_cast<size_t>((c * mipFactor))];
												for (size_t L = 0; L < nLevels; ++L)
												{
													const float m = vmips[
														L * plane
														+ static_cast<size_t>(r) * static_cast<size_t>(mapSide)
														+ static_cast<size_t>(c)];
													const float deficit = corner - m;
													if (deficit > kFloorTol)
													{
														worstDeficit = deficit;
														++viol;
														break;
													}
												}
												if (viol) break;
											}
										}
										if (viol > 0)
										{
											mc2::fmt::print_out("[VISUAL_HEIGHT v1] mips STALE path={} (mip < fine corner, worst deficit {:.2f} wu) -- built from a DIFFERENT fine bake; DROPPING mips, coarse bands fall back to legacy S2. Regenerate the .beauty bake.\n", mc2::fmt::safe(mipPath), worstDeficit);
											fflush(stdout);
											vmips.clear();
										}
									}
									fclose(mf);
								}
							}
						}
						// TERRAIN-HIRES-NEAR-1 mod guard: the bake is drawn by default now, so a
						// bake that does not belong to this mission's gameplay heights (a mod that
						// reshapes the terrain under a stock-named .beauty sidecar) must not draw.
						// Stock bakes move their coarse corners by ~2 wu on average; reject when
						// the mean corner offset exceeds 16 wu (terrain stays undisplaced).
						double cornerSum = 0.0;
						size_t cornerCnt = 0;
						for (int cy = 0; cy < mapSide && cy * 4 < V; ++cy)
							for (int cx = 0; cx < mapSide && cx * 4 < V; ++cx, ++cornerCnt)
								cornerSum += fabs(static_cast<double>(vh[static_cast<size_t>(cx) * 4 + static_cast<size_t>(cy) * 4 * V])
								                  - static_cast<double>(elev[static_cast<size_t>(cx) + static_cast<size_t>(cy) * mapSide]));
						const double cornerMean = cornerCnt ? cornerSum / static_cast<double>(cornerCnt) : 0.0;
						// TERRAIN-BEAUTY-SIG-1: exact binding of the bake to the gameplay grid.
						// <beauty>/gameplay_height.sig = "<mapSide> <fnv1a64 hex>" over (mapSide u32,
						// then mapSide^2 float32 elevations). Present: apply only on an exact match
						// (a mod that edits one plateau passes the mean test above, not this).
						// Absent (mod-shipped sidecar): the legacy mean guard still applies.
						const int sigState = s_beautySig.state;
						const char* const sigWant = s_beautySig.want;
						if (sigState == 1 && mc2_diag::tagRequested("VISUAL_HEIGHT"))   // success info: opt-in; mismatches stay fail-loud
						{
							mc2::fmt::print_out("[VISUAL_HEIGHT v1] SIG OK path={} gameplay height signature '{}' matches; bake applied\n", mc2::fmt::safe(vhPath), mc2::fmt::safe(sigWant));
							fflush(stdout);
						}
						if (sigState == 0 && cornerMean > 16.0)
						{
							mc2::fmt::print_out("[VISUAL_HEIGHT v1] REJECTED path={} mean corner offset {:.1f}wu > 16wu: bake does not match this terrain (mod?); drawing undisplaced\n", mc2::fmt::safe(vhPath), cornerMean);
							fflush(stdout);
							return;
						}
						{
						gos_TerrainLodChunk_UploadVisualHeightFull(
							vh.data(), V,
							vmips.empty() ? nullptr : vmips.data(),
							static_cast<int>(vmips.size()));
						TerrainRuntime::setCpuVisualHeight(vh.data(), V);   // UNIT-VISUAL-SNAP-1 (render-only)
						}
						// TERRAIN-DISPLACEMENT-TRUTH-1: report the REAL state, not
						// the stale Stage-1 lie. Stage 2 shipped: when
						// MC2_TERRAIN_VISUAL_DISPLACE is on the vert shader
						// (terrain_lod_chunk.vert) samples binding 26 and MOVES
						// geometry (proven by a checkerboard pixel oracle). The old
						// "(Stage 1: SSBO only, geometry unchanged)" text is FALSE
						// under the displace gate and caused the pipeline to be
						// repeatedly (mis)diagnosed as broken. Also surface the
						// actual displacement amplitude vs the coarse gameplay grid
						// so an invisible-because-too-subtle bake is obvious at load
						// time instead of after four viewing runs.
						const bool displaceOn = mc2::gate::get<"MC2_TERRAIN_VISUAL_DISPLACE">();
						double maxAbs = 0.0, sumAbs = 0.0;
						size_t moved5 = 0, moved50 = 0, cnt = 0;
						// Compare every fine bake vertex to the coarse gameplay
						// elevation at its containing coarse cell (the "blocky"
						// baseline the displacement smooths away). elev[] is the
						// mapSide*mapSide coarse grid loaded above (row-major).
						for (int fy = 0; fy < V; ++fy)
							for (int fx = 0; fx < V; ++fx)
							{
								int cx = fx / 4; cx = std::min(cx, mapSide - 1);
								int cy = fy / 4; cy = std::min(cy, mapSide - 1);
								double d = fabs(static_cast<double>(vh[static_cast<size_t>(fx) + static_cast<size_t>(fy) * V])
								                - static_cast<double>(elev[static_cast<size_t>(cx) + static_cast<size_t>(cy) * mapSide]));
								maxAbs = std::max(maxAbs, d);
								sumAbs += d; ++cnt;
								if (d > 5.0)  ++moved5;
								if (d > 50.0) ++moved50;
							}
						const double meanAbs = cnt ? sumAbs / static_cast<double>(cnt) : 0.0;
						const double frac5  = cnt ? (100.0 * static_cast<double>(moved5)  / static_cast<double>(cnt)) : 0.0;
						const double frac50 = cnt ? (100.0 * static_cast<double>(moved50) / static_cast<double>(cnt)) : 0.0;
						if (mc2_diag::tagRequested("VISUAL_HEIGHT")) {   // W115 quiet: load summary is opt-in (MC2_DIAG_TAGS=VISUAL_HEIGHT)
						mc2::fmt::print_out("[VISUAL_HEIGHT v1] LOADED path={} V={:d} mapSide={:d} bytes={:d} (Stage 2: geometry {} when MC2_TERRAIN_VISUAL_DISPLACE on)\n", mc2::fmt::safe(vhPath), mc2::fmt::as_i32(V), mc2::fmt::as_i32(mapSide), mc2::fmt::as_size(want), mc2::fmt::safe(displaceOn ? "DISPLACED" : "will displace"));
						mc2::fmt::print_out("[VISUAL_HEIGHT v1] displacement-amplitude vs coarse grid: max={:.1f}wu mean={:.2f}wu moved>5wu={:.1f}% moved>50wu={:.2f}% (vertex spacing=128wu; if mean<~5wu the reshape is near-invisible -- re-bake with visual_heightfield.py --reshape)\n", maxAbs, meanAbs, frac5, frac50);
						fflush(stdout);
						}

						// TERRAIN-REAUTH-UNPIN-1 Half B: near-object displacement
						// fade (objfade). Load the static damp sidecar
						// (visual_damp.r32, mapSide^2 floats, 0 = displacement OFF
						// on/near building footprints) from the same .beauty dir;
						// absent -> all-ones (movers-only fade). Gate
						// MC2_TERRAIN_VISUAL_DISPLACE_OBJFADE default ON when
						// displacing (it is the safety). Also logs the
						// grounding-drift evidence: p99 EFFECTIVE displacement
						// (|bake - gameplay| * damp) at coarse cells, split into
						// the inner (damp==0) zone — must be exactly 0 — and the
						// fade annulus.
						{
							static const bool s_objfadeGate = mc2::gate::get<"MC2_TERRAIN_VISUAL_DISPLACE_OBJFADE">();
							if (s_objfadeGate)
							{
								std::filesystem::path dampFs(vhPath);
								if (dampFs.has_parent_path())
									dampFs.replace_filename("visual_damp.r32");
								const std::string dampPathStr = dampFs.string();
								const char* dampPath = dampPathStr.c_str();
								std::vector<float> damp(static_cast<size_t>(mapSide) * static_cast<size_t>(mapSide), 1.0f);
								bool haveSidecar = false;
								if (FILE* df = fopen(dampPath, "rb"))
								{
									fseek(df, 0, SEEK_END);
									long dsz = ftell(df);
									fseek(df, 0, SEEK_SET);
									if (static_cast<size_t>(dsz) == damp.size() * sizeof(float))
									{
										haveSidecar =
											(fread(damp.data(), sizeof(float), damp.size(), df)
											 == damp.size());
									}
									fclose(df);
									if (!haveSidecar)
									{
										mc2::fmt::print_out("[VISUAL_DAMP v1] SIZE/READ MISMATCH path={} got={:d} want={:d} -- movers-only fade\n", mc2::fmt::safe(dampPath), mc2::fmt::as_i32(dsz), mc2::fmt::as_size(damp.size() * sizeof(float)));
										std::fill(damp.begin(), damp.end(), 1.0f);
									}
								}
								else
								{
									mc2::fmt::print_out("[VISUAL_DAMP v1] sidecar NOT FOUND path={} -- movers-only fade\n", mc2::fmt::safe(dampPath));
								}
								gos_TerrainLodChunk_UploadVisualDampStatic(damp.data(), mapSide);
								// Grounding-drift evidence (the objfade acceptance
								// number): effective displacement per coarse cell.
								std::vector<double> inner, annulus;
								double innerMax = 0.0;
								for (int cy = 0; cy < mapSide; ++cy)
									for (int cx = 0; cx < mapSide; ++cx)
									{
										const size_t ci = static_cast<size_t>(cx) + static_cast<size_t>(cy) * mapSide;
										const size_t fi = static_cast<size_t>((cx * 4))
											+ static_cast<size_t>((cy * 4)) * static_cast<size_t>(V);
										const double full = fabs(static_cast<double>(vh[fi]) - static_cast<double>(elev[ci]));
										const double d = static_cast<double>(damp[ci]);
										const double eff = full * d;
										if (d <= 0.0)
										{
											inner.push_back(eff);
											innerMax = std::max(innerMax, eff);
										}
										else if (d < 0.999)
											annulus.push_back(eff);
									}
								auto p99 = [](std::vector<double>& v2) -> double {
									if (v2.empty()) return 0.0;
									std::sort(v2.begin(), v2.end());
									return v2[static_cast<size_t>((static_cast<double>((v2.size() - 1)) * 0.99))];
								};
								const double innerP99 = p99(inner);
								const double annP99 = p99(annulus);
								if (mc2_diag::tagRequested("VISUAL_HEIGHT"))   // W115 quiet: drift summary is opt-in; size/missing lines above stay fail-loud
								mc2::fmt::print_out("[VISUAL_DAMP v1] {} side={:d} objfade drift-near-objects: inner(damp==0) cells={:d} p99={:.3f}wu max={:.3f}wu (must be ~0) fade-annulus cells={:d} p99={:.2f}wu\n", mc2::fmt::safe(haveSidecar ? "LOADED" : "ALL-ONES (no sidecar)"), mc2::fmt::as_i32(mapSide), mc2::fmt::as_size(inner.size()), innerP99, innerMax, mc2::fmt::as_size(annulus.size()), annP99);
								fflush(stdout);
							}
						}
					}
				}
			}
		}
	}
}

// LOD chunk: flag the blocks that contain concrete so the LOD pass keeps them fine.
void Terrain::lodFlagConcrete(TerrainLodInitFrame& lf) {
	auto& ttype = lf.ttype;
	// Step 5c: flag blocks containing concrete (material 3). The cement
	// word is per-tile; a coarse-LOD triangle spans several tiles and gets
	// one tile's word -> torn runways. The LOD pass clamps these blocks fine.
	const int side = static_cast<int>(realVerticesMapSide);
	for (int by = 0; by < s_terrainChunkSide; ++by)
		for (int bx = 0; bx < s_terrainChunkSide; ++bx)
		{
			TerrainBlockMeta& bm = s_blockMeta[bx + by * s_terrainChunkSide];
			bm.hasConcrete = false;
			const int ox = bx * 20, oy = by * 20;
			for (int ly = 0; ly <= 20 && !bm.hasConcrete; ++ly)
				for (int lx = 0; lx <= 20; ++lx)
				{
					const int mx = ox + lx, my = oy + ly;
					if (mx >= side || my >= side) continue;
					if (ttype[static_cast<size_t>(mx) + static_cast<size_t>(my) * side] >= 2.5f) { bm.hasConcrete = true; break; }
				}
		}
}

// Allocate the vertex and quad lists and the map cell arrays.
void Terrain::initVertexQuadLists(unsigned long visibleVertices) {
	//----------------------------------------------------------------------
	// Create the VertexList
	numberVertices = 0;
	vertexList = static_cast<VertexPtr>(std::malloc(sizeof(Vertex) * visibleVertices * visibleVertices));
	gosASSERT(vertexList != nullptr);
	memset(vertexList,0,sizeof(Vertex) * visibleVertices * visibleVertices);

	//----------------------------------------------------------------------
	// Create the QuadList
	numberQuads = 0;
	quadList = static_cast<TerrainQuadPtr>(std::malloc(sizeof(TerrainQuad) * visibleVertices * visibleVertices));
	gosASSERT(quadList != nullptr);
	memset(quadList,0,sizeof(TerrainQuad) * visibleVertices * visibleVertices);

	//-------------------------------------------------------------------
	initMapCellArrays();
}

// Start loading the colour map when it exists.
void Terrain::initColorMapLoad() {
	//-----------------------------------------------------------------
	// Startup the Terrain Color Map
	if ( terrainTextures2  && !(terrainTextures2->colorMapStarted))
	{
		const auto colorMapStart = std::chrono::steady_clock::now();
		if (colorMapName)
			terrainTextures2->init(colorMapName);
		else
			terrainTextures2->init(terrainName);
		if (mc2_diag::tagRequested("LOAD_TIMING"))
			mc2::fmt::print_out("[LOAD_TIMING] load=TerrainColorMap::init elapsed_ms={:.3f}\n",
				std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - colorMapStart).count());
	}
}

// Select the terrain material profile for this mission.
void Terrain::initMaterialProfile() {
	//-----------------------------------------------------------------
	// C1 tactical material profile selection. Whitelist-only; default
	// LEGACY preserves byte-for-byte pre-C1 rendering on every other
	// mission. Profile is read by gos_terrain.frag / terrain_common.hglsl
	// classifier branches via the `g_terrainMaterialProfile` uniform.
	{
		const char *profileKey = colorMapName ? colorMapName
		                       : terrainName  ? terrainName
		                                      : "";
		g_terrainMaterialProfile = static_cast<int>(TERRAIN_MAT_PROFILE_LEGACY);
		// TERRAIN-CLASSIFY-TUNING-1: sync dirt sat window with profile so the
		// ImGui-tunable uniforms start at the right defaults for this mission.
		// Sand_M24 washes out to low-saturation sand; widen the dirt sat gate.
		extern void gos_SetTerrainClassDirt(float rMinusGLo, float rMinusGHi, float rBrightLo, float rBrightHi);
		if (profileKey[0] != '\0' && _stricmp(profileKey, "mc2_24") == 0) {
			g_terrainMaterialProfile = static_cast<int>(TERRAIN_MAT_PROFILE_SAND_M24);
			// Sand_M24: widen R-G band + raise brightness ceiling for bright sun-lit sand
			gos_SetTerrainClassDirt(-0.02f, 0.12f, 0.18f, 0.80f);
		} else {
			gos_SetTerrainClassDirt(-0.02f, 0.06f, 0.22f, 0.45f);
		}
	}
}

// Mission terrain load: heap, textures, block lists, map data, the optional LOD-chunk metadata and
// sidecar uploads, then the vertex / quad lists and the colour-map and material-profile setup.
// Each phase is a member helper declared in terrain.h, in execution order.
long Terrain::init( unsigned long verticesPerMapSide, PacketFile* pakFile, unsigned long visibleVertices,
				   volatile float& percent,
					float percentRange)
{
	ZoneScopedN("Terrain::init");
	// Derived color never survives a mission switch. Source loaders below retain their provider order.
	mc2terrain::setSurfaceMissionCook({});
	if (auto* fields = gos_TerrainFieldStore()) fields->clearSurfaceAsset();
	initMapExtents(verticesPerMapSide, visibleVertices);
	initTerrainHeap(percent, percentRange);
	initTerrainTextures(pakFile, percent, percentRange);

	if ( !pakFile && !realVerticesMapSide )
		return NO_ERR;

	initColorMapTexture(pakFile, percent, percentRange);
	initObjBlockLists(percent, percentRange);
	initMapData(pakFile, percent, percentRange);

	//----------------------------------------------------------------------
	// Terrain LOD chunk Phase 1 — allocate per-block + superchunk metadata.
	// MC2_TERRAIN_LOD_CHUNK=1 gate. mapData must be live before this block.
	if (mc2TerrainLodChunkEnabled()) {
		lodAllocMeta();

		// Terrain LOD chunk Phase 3 — upload full heightfield to GPU SSBO.
		{
			TerrainLodInitFrame lf;
			lodHeightUpload(lf);
			s_beautySig = computeBeautySig(terrainName ? terrainName : "",static_cast<int>(realVerticesMapSide), lf.elev);
			lodControlMap();
			lodOverlayV2();
			lodShorelineMask();
			lodWaterShore();
			lodVisualHeight(lf);
			lodFlagConcrete(lf);
		}
	}

	initVertexQuadLists(visibleVertices);
	initColorMapLoad();
	initMaterialProfile();
	// The atlas is assembled later, during primeMissionTerrainCache. Keep only the optional
	// resolved cache here; the native importer validates it against the final atlas and shore bytes.
	{
		std::string stem = terrainName ? terrainName : "";
		const auto slash = stem.find_last_of("/\\");
		if (slash != std::string::npos) stem.erase(0, slash + 1);
		if (const auto dot = stem.find_last_of('.'); dot != std::string::npos) stem.erase(dot);
		const std::string path = "data/missions/" + stem + ".beauty/macro_color.tsf";
		File cache;
		if (cache.open(path.c_str()) == NO_ERR) {
			const auto length = cache.getLength();
			if (length > 0 && static_cast<uint64_t>(length) <= mc2terrain::SurfaceAsset::kMaxBytes) {
				std::vector<uint8_t> bytes(static_cast<size_t>(length));
				if (cache.read(bytes.data(), length) == static_cast<long>(length))
					mc2terrain::setSurfaceMissionCook(std::move(bytes));
			}
		}
	}

	return NO_ERR;
}
