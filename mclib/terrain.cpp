//---------------------------------------------------------------------------
//
// Terrain.cpp -- File contains calss definitions for the Terrain
//
//	MechCommander 2
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

//---------------------------------------------------------------------------
// Include Files

#include "terrain_internal.h"
#include "mc2fmt.h"


//---------------------------------------------------------------------------
// Static Globals
float worldUnitsPerMeter = 5.01f;
float metersPerWorldUnit = 0.2f;
long terrainLineChanged = 0;

MapDataPtr					Terrain::mapData = nullptr;
TerrainTexturesPtr			Terrain::terrainTextures = nullptr;
TerrainColorMapPtr			Terrain::terrainTextures2 = nullptr;

const long					Terrain::verticesBlockSide = 20;			//Changes for new terrain?
long						Terrain::blocksMapSide = 0;					//Calced during load.

long						Terrain::visibleVerticesPerSide = 0;		//Passed in.

const float					Terrain::worldUnitsPerVertex = 128.0;
const float					Terrain::worldUnitsPerCell = Terrain::worldUnitsPerVertex / MAPCELL_DIM;
const float					Terrain::halfWorldUnitsPerCell = Terrain::worldUnitsPerCell / 2.0f;
const float					Terrain::metersPerCell = Terrain::worldUnitsPerCell * metersPerWorldUnit;
const float					Terrain::worldUnitsBlockSide = Terrain::worldUnitsPerVertex * Terrain::verticesBlockSide;
const float					Terrain::oneOverWorldUnitsPerVertex = 1.0f / Terrain::worldUnitsPerVertex;
const float					Terrain::oneOverWorldUnitsPerCell = 1.0f / Terrain::worldUnitsPerCell;
const float					Terrain::oneOverMetersPerCell = 1.0f / Terrain::metersPerCell;
const float					Terrain::oneOverVerticesBlockSide = 1.0f / Terrain::verticesBlockSide;

float						Terrain::worldUnitsMapSide = 0.0;		//Calced during load.
float						Terrain::oneOverWorldUnitsMapSide = 0.0f;
long						Terrain::halfVerticesMapSide = 0;
long						Terrain::realVerticesMapSide = 0;

glm::vec3				Terrain::mapTopLeft3d;					//Calced during load.

char *						Terrain::terrainName = nullptr;
char * 						Terrain::colorMapName = nullptr;
std::unique_ptr<MapData> Terrain::mapDataOwner;
std::unique_ptr<TerrainTextures> Terrain::terrainTexturesOwner;
std::unique_ptr<TerrainColorMap> Terrain::terrainTextures2Owner;
std::unique_ptr<char[]> Terrain::terrainNameOwner;
std::unique_ptr<char[]> Terrain::colorMapNameOwner;

// C1 tactical material profile (see terrain.h). Default LEGACY = exact
// pre-C1 byte-for-byte behavior; only mc2_24 currently flips to a
// non-LEGACY profile.
int							g_terrainMaterialProfile = TERRAIN_MAT_PROFILE_LEGACY;

// Terrain LOD chunk renderer gate — DEFAULT ON (cutover 2026-06-09). Opt out with
// MC2_TERRAIN_LOD_CHUNK=0. Cached once. Single source of truth (see terrain.h).
bool mc2TerrainLodChunkEnabled()
{
	static const bool s_on = []() {
		const bool off = !mc2::gate::get<"MC2_TERRAIN_LOD_CHUNK">();
		if (off)
			mc2::fmt::print_out("[8Z_VESTIGIAL] MC2_TERRAIN_LOD_CHUNK=0 opt-out is increasingly vestigial: slimReduce deleted (8z-A3), ACTIVE_AB/SOLID_AB retired (8z-A1). The chunk-OFF path retains makeLists but has no production renderer.\n");
		return !off;   // default ON; "0" = off
	}();
	return s_on;
}

long		   				Terrain::numObjBlocks = 0;
ObjBlockInfo				*Terrain::objBlockInfo = nullptr;
bool						*Terrain::objVertexActive = nullptr;

float 						*Terrain::tileRowToWorldCoord = nullptr;
float 						*Terrain::tileColToWorldCoord = nullptr;
float 						*Terrain::cellToWorldCoord = nullptr;
float 						*Terrain::cellColToWorldCoord = nullptr;
float 						*Terrain::cellRowToWorldCoord = nullptr;

float 						Terrain::waterElevation = 0.0f;
float						Terrain::frameAngle = 0.0f;
float 						Terrain::frameCos = 1.0f;
float						Terrain::frameCosAlpha = 1.0f;
DWORD						Terrain::alphaMiddle = 0xaf000000;
DWORD						Terrain::alphaEdge = 0x3f000000;
DWORD						Terrain::alphaDeep = 0xff000000;
float						Terrain::waterFreq = 4.0f;
float						Terrain::waterAmplitude = 10.0f;

long						Terrain::userMin = 0;
long						Terrain::userMax = 0;
unsigned long				Terrain::baseTerrain = 0;
unsigned char				Terrain::fractalThreshold = 1;
unsigned char				Terrain::fractalNoise = 0;
bool						Terrain::recalcShadows = false;
bool						Terrain::recalcLight = false;

// Terrain LOD chunk Phase 1 static members (MC2_TERRAIN_LOD_CHUNK=1 gate).
TerrainBlockMeta*			Terrain::s_blockMeta       = nullptr;
SuperchunkMeta*				Terrain::s_superchunkMeta  = nullptr;
TerrainDrawCommand*			Terrain::s_drawCmds        = nullptr;
std::unique_ptr<float[]>		Terrain::s_skirtDepths;
int							Terrain::s_cmdCount        = 0;
unsigned long				Terrain::gCurrentFrame     = 0;
int							Terrain::s_terrainChunkSide = 0;
int							Terrain::s_superchunkSide   = 0;

// Phase 10.2b: per-draw-command skirt EDGE mask (bit 0=N,1=S,2=W,3=E). Parallel
// to s_drawCmds / s_skirtDepths; sized to the block count, written in Pass 3 so
// the driver draws a skirt only on edges whose neighbour LOD differs.
std::vector<uint8_t> s_skirtEdgeMaskVec;

// Phase 10.4: per-draw-command edge-STITCH packing. For each edge bordering a
// COARSER visible neighbour, pack that neighbour's vertex stride into a byte
// (N=bits0-7, S=8-15, W=16-23, E=24-31; 0 = no stitch). The vertex shader snaps
// the fine edge's intermediate verts onto the coarse edge line -> bit-identical
// shared edge -> no T-junction crack (neighbour-min technique, cf c2d7eb46).
std::vector<uint32_t> s_stitchStepVec;

bool 						drawTerrainGrid = false;		//Override locally in editor so game don't come with these please!  Love -fs
bool						drawEditorPassability = false;	// Editor passability overlay — MOVE-free path for large maps
uint8_t*					gEditorNavFlags    = nullptr;	// Set by EditorNavLayer::BuildFromTerrain()
int						gEditorNavCellSide = 0;		// terrain cellSide = realVerticesMapSide - 1
bool						drawLOSGrid = false;
bool						drawTerrainTiles = true;
bool						drawTerrainOverlays = true;
bool						drawTerrainMines = true;
bool						renderObjects = true;
bool						renderTrees = true;

TerrainPtr					land = nullptr;

long 						*usedBlockList;					//Used to determine what objects to deal with.
long 						*moverBlockList;

unsigned long 				blockMemSize = 0;				//Misc Flags.
bool 						useOldProject = FALSE;
bool 						projectAll = FALSE;
bool 						useFog = true;
bool 						useVertexLighting = true;
bool 						useFaceLighting = false;

unsigned char 				godMode = 0;			//Can I simply see everything, enemy and friendly?

long						visualRangeTable[256];
// NS3 boundary: justResaveAllMaps canonical def (was code/ leak; editor consumes it).
bool justResaveAllMaps = false;
//---------------------------------------------------------------------------
// These are used to determine what terrain objects to process.
// They date back to GenCon 1996!!
void addBlockToList (long blockNum)
{
	long totalBlocks = Terrain::blocksMapSide * Terrain::blocksMapSide;
	for (long i=0;i<totalBlocks;i++)
	{
		if (usedBlockList[i] == blockNum)
		{
			return;
		}
		else if (usedBlockList[i] == -1)
		{
			usedBlockList[i] = blockNum;
			return;
		}
	}
}

//---------------------------------------------------------------------------
void addMoverToList (long blockNum)
{
	long totalBlocks = Terrain::blocksMapSide * Terrain::blocksMapSide;
	for (long i=0;i<totalBlocks;i++)
	{
		if (moverBlockList[i] == blockNum)
		{
			return;
		}
		else if (moverBlockList[i] == -1)
		{
			moverBlockList[i] = blockNum;
			return;
		}
	}
}

//---------------------------------------------------------------------------
void clearList (void)
{
	long totalBlocks = Terrain::blocksMapSide * Terrain::blocksMapSide;
	blockMemSize = totalBlocks * sizeof(long);
	
	if (usedBlockList)
		memset(usedBlockList,-1,blockMemSize);
}

//---------------------------------------------------------------------------
void clearMoverList (void)
{
	long totalBlocks = Terrain::blocksMapSide * Terrain::blocksMapSide;
	blockMemSize = totalBlocks * sizeof(long);
	
	if (moverBlockList)
		memset(moverBlockList,-1,blockMemSize);
}

//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
// Terrain LOD chunk Phase 1 static helpers.
// Only called when MC2_TERRAIN_LOD_CHUNK is set; all access already gated.

void recomputeBlockAabb(TerrainBlockMeta& bm)
{
    // Scan the (quadCountX+1) x (quadCountY+1) inclusive vertex footprint.
    // OOB vertices (past map edge) use blankVertex elevation (33.0f).
    float mn =  1e30f;
    float mx = -1e30f;
    int mapSide = static_cast<int>(Terrain::realVerticesMapSide);
    PostcompVertexPtr blks = Terrain::mapData->getBlocks();
    for (int dy = 0; dy <= bm.quadCountY; ++dy) {
        for (int dx = 0; dx <= bm.quadCountX; ++dx) {
            int vx = bm.originX + dx;
            int vy = bm.originY + dy;
            float elev;
            if (vx >= mapSide || vy >= mapSide)
                elev = 33.0f;   // blankVertex elevation (mapdata.cpp:641)
            else
                elev = blks[vx + vy * mapSide].elevation;
            if (elev < mn) mn = elev;
            if (elev > mx) mx = elev;
        }
    }
    bm.minElev   = mn;
    bm.maxElev   = mx;
    bm.dirtyAabb = false;
}

void recomputeSuperchunkAabb(int scX, int scY)
{
    SuperchunkMeta& sc = Terrain::s_superchunkMeta[scX + scY * Terrain::s_superchunkSide];
    float xMin =  1e30f, xMax = -1e30f;
    float yMin =  1e30f, yMax = -1e30f;
    float zMin =  1e30f, zMax = -1e30f;

    float halfMap = Terrain::worldUnitsMapSide * 0.5f;

    for (int dy = 0; dy < 4; ++dy) {
        for (int dx = 0; dx < 4; ++dx) {
            int bx = scX * 4 + dx, by = scY * 4 + dy;
            if (bx >= Terrain::s_terrainChunkSide || by >= Terrain::s_terrainChunkSide) continue;
            const TerrainBlockMeta& bm =
                Terrain::s_blockMeta[bx + by * Terrain::s_terrainChunkSide];
            float wMinX =  float(bm.originX)                  * 128.0f - halfMap;
            float wMaxX =  float(bm.originX + bm.quadCountX)  * 128.0f - halfMap;
            float wMaxY =  halfMap - float(bm.originY)                  * 128.0f;
            float wMinY =  halfMap - float(bm.originY + bm.quadCountY)  * 128.0f;
            if (wMinX < xMin) xMin = wMinX;
            if (wMaxX > xMax) xMax = wMaxX;
            if (wMinY < yMin) yMin = wMinY;
            if (wMaxY > yMax) yMax = wMaxY;
            if (bm.minElev < zMin) zMin = bm.minElev;
            if (bm.maxElev > zMax) zMax = bm.maxElev;
        }
    }
    sc.worldMinX = xMin; sc.worldMaxX = xMax;
    sc.worldMinY = yMin; sc.worldMaxY = yMax;
    // Conservative z-range matching old frustum path.
    sc.worldMinZ = -200.0f;
    sc.worldMaxZ = 2500.0f;
    sc.inFrustum = false;
}

//---------------------------------------------------------------------------
// class Terrain
void Terrain::init (void)
{
	vertexList = nullptr;
	numberVertices = 0;
	
	quadList = nullptr;
	numberQuads = 0;
}

//---------------------------------------------------------------------------
void Terrain::initMapCellArrays (void)
{
	if (!tileRowToWorldCoord)
	{
		tileRowToWorldCoord = static_cast<float *>(std::malloc(sizeof(float) * realVerticesMapSide));
		gosASSERT(tileRowToWorldCoord != nullptr);
	}

	if (!tileColToWorldCoord)
	{
		tileColToWorldCoord = static_cast<float *>(std::malloc(sizeof(float) * realVerticesMapSide));
		gosASSERT(tileColToWorldCoord != nullptr);
	}

	if (!cellToWorldCoord)
	{
		cellToWorldCoord = static_cast<float *>(std::malloc(sizeof(float) * MAPCELL_DIM));
		gosASSERT(cellToWorldCoord != nullptr);
	}

	if (!cellColToWorldCoord)
	{
		cellColToWorldCoord = static_cast<float *>(std::malloc(sizeof(float) * realVerticesMapSide * MAPCELL_DIM));
		gosASSERT(cellColToWorldCoord != nullptr);
	}

	if (!cellRowToWorldCoord)
	{
		cellRowToWorldCoord = static_cast<float *>(std::malloc(sizeof(float) * realVerticesMapSide * MAPCELL_DIM));
		gosASSERT(cellRowToWorldCoord != nullptr);
	}

	long i=0;

	long height = realVerticesMapSide, width = height;
	for (i = 0; i < height; i++)
		tileRowToWorldCoord[i] = (worldUnitsMapSide / 2.0) - (i * worldUnitsPerVertex);

	for (i = 0; i < width; i++)
		tileColToWorldCoord[i] = (i * worldUnitsPerVertex) - (worldUnitsMapSide / 2.0);

	for (i = 0; i < MAPCELL_DIM; i++)
		cellToWorldCoord[i] = (worldUnitsPerVertex / static_cast<float>(MAPCELL_DIM)) * i;

	long maxCell = height * MAPCELL_DIM;
	for (i = 0; i < maxCell; i++)
		cellRowToWorldCoord[i] = (worldUnitsMapSide / 2.0) - (i * worldUnitsPerCell);

	maxCell = width * MAPCELL_DIM;
	for (i = 0; i < maxCell; i++)
		cellColToWorldCoord[i] = (i * worldUnitsPerCell) - (worldUnitsMapSide / 2.0);
}	

//---------------------------------------------------------------------------
long Terrain::init (PacketFile* pakFile, int whichPacket, unsigned long visibleVertices, volatile float& percent,
					float percentRange )
{
	clearList();
	clearMoverList();
	
	long result = pakFile->seekPacket( whichPacket );
	if (result != NO_ERR)
		STOP(("Unable to seek Packet %d in file %s",whichPacket,pakFile->getFilename()));
	
	int tmp = pakFile->getPacketSize();
	realVerticesMapSide = sqrt( float(tmp/ sizeof(PostcompVertex)));
	
	if (!justResaveAllMaps)
	{
		if (realVerticesMapSide < 60 || realVerticesMapSide > 2048)
			STOP(("Terrain grid size %d out of supported range [60, 2048]", realVerticesMapSide));
		// Check quads (vertices-1), not vertices, so partial-edge blocks are permitted
		// under MC2_TERRAIN_LOD_CHUNK=1 (e.g. 120-vertex map: 119 quads, last block = 19).
		if ((realVerticesMapSide - 1) % verticesBlockSide != 0) {
			if (!mc2TerrainLodChunkEnabled())
				STOP(("Terrain quad count %d not divisible by verticesBlockSide (%d)",
				      realVerticesMapSide - 1, verticesBlockSide));
			// Partial-edge blocks permitted under MC2_TERRAIN_LOD_CHUNK=1.
			// quadCountX = min(verticesBlockSide, (realVerticesMapSide-1) - originX).
		}
	}
	
	init( realVerticesMapSide, pakFile, visibleVertices, percent, percentRange );	
	
	return(NO_ERR);
}

//---------------------------------------------------------------------------
void Terrain::getColorMapName (FitIniFile *file)
{
	if (file)
	{
		if (file->seekBlock("ColorMap") == NO_ERR)
		{
			char mapName[1024];
			if (file->readIdString("ColorMapName",mapName,1023) == NO_ERR)
			{
				colorMapNameOwner = std::make_unique<char[]>(strlen(mapName)+1);
				strcpy(colorMapNameOwner.get(),mapName);
				colorMapName = colorMapNameOwner.get();
				return;
			}
		}
	}

	colorMapNameOwner.reset();
	colorMapName = nullptr;
}

//---------------------------------------------------------------------------
void Terrain::setColorMapName (char *mapName)
{
	colorMapNameOwner.reset();
	colorMapName = nullptr;

	if (mapName)
	{
		colorMapNameOwner = std::make_unique<char[]>(strlen(mapName)+1);
		strcpy(colorMapNameOwner.get(),mapName);
		colorMapName = colorMapNameOwner.get();
	}
}

//---------------------------------------------------------------------------
void Terrain::saveColorMapName (FitIniFile *file)
{
	if (file && colorMapName)
	{
		file->writeBlock("ColorMap");
		file->writeIdString("ColorMapName",colorMapName);
	}
}

void Terrain::resetVisibleVertices (long maxVisibleVertices)
{
	std::free(vertexList);
	vertexList = nullptr;

	std::free(quadList);
	quadList = nullptr;

	visibleVerticesPerSide = maxVisibleVertices;
	//----------------------------------------------------------------------
	// Create the VertexList
	numberVertices = 0;
	vertexList = static_cast<VertexPtr>(std::malloc(sizeof(Vertex) * visibleVerticesPerSide * visibleVerticesPerSide));
	gosASSERT(vertexList != nullptr);
	memset(vertexList,0,sizeof(Vertex) * visibleVerticesPerSide * visibleVerticesPerSide);

	//----------------------------------------------------------------------
	// Create the QuadList
	numberQuads = 0;
	quadList = static_cast<TerrainQuadPtr>(std::malloc(sizeof(TerrainQuad) * visibleVerticesPerSide * visibleVerticesPerSide));
	gosASSERT(quadList != nullptr);
	memset(quadList,0,sizeof(TerrainQuad) * visibleVerticesPerSide * visibleVerticesPerSide);

	
}

//---------------------------------------------------------------------------
void Terrain::primeMissionTerrainCache (volatile float& progress, float progressRange)
{
	if (!mapData || !terrainTextures2)
		return;

	const float buildRange = progressRange * 0.5f;
	const float warmRange = progressRange - buildRange;
	{
		ZoneScopedN("Terrain::primeMissionTerrainCache build");
		mapData->buildTerrainFaceCache(&progress, buildRange);
	}
	{
		ZoneScopedN("Terrain::primeMissionTerrainCache warm");
		mapData->warmTerrainFaceCacheResidency(&progress, warmRange);
	}

	// Stage 2 of the renderWater architectural slice (CPU→GPU offload):
	// build the static, map-keyed WaterRecipe array. Iterates MapData::blocks
	// directly (mission-immutable) — independent of quadList which is
	// camera-windowed and reshuffles each frame. Spec:
	// docs/superpowers/specs/2026-04-29-renderwater-fastpath-design.md.
	{
		ZoneScopedN("Terrain::primeMissionTerrainCache water_stream_build");
		WaterStream::Reset();
		WaterStream::Build();
	}

	// Stage 2 of the indirect-terrain SOLID-only PR1 (CPU→GPU offload):
	// build the dense TerrainQuadRecipe array (mapSide² × 144 B) indexed by
	// vertexNum. Called AFTER buildTerrainFaceCache (line 585) so the Shape C
	// cache is ready when buildRecipeSlot reads UV data from it.
	// Gated on IsEnabled() OR IsParityCheckEnabled() — no allocation when both
	// are unset.
	if (gos_terrain_indirect::IsEnabled() ||
	    gos_terrain_indirect::IsParityCheckEnabled()) {
		gos_terrain_indirect::ResetDenseRecipe();
		gos_terrain_indirect::BuildDenseRecipe();
	}

	// PR2c Stage 1c — mine static-bake lifecycle.
	// CPU-clear only; do NOT build here. Build is deferred to first
	// MissionMap::setMine event (typically the per-cell init loop at
	// move.cpp:991) followed by a paint-cycle invocation of
	// RebuildMineStaticVBOIfDirty from the Stage 2c bridge. This avoids
	// the R7 timing trap (mineTextureHandle/blownTextureHandle are still
	// 0xffffffff at primeMissionTerrainCache time — they lazy-load only
	// when TerrainQuad::setupTextures fires per-quad in the first paint
	// cycle).
	gos_terrain_indirect::ResetMineStaticVBO();
	gos_terrain_indirect::ResetMineTextureArray();
	// A5 / R7 fix: load the mine + scorch texture HANDLES now (NOT the VBO — that stays
	// deferred per the comment above to avoid the R7 build-timing trap). This decouples
	// handle availability from TerrainQuad::setupTextures so mines still render after
	// Phase 8z removes the per-quad setupTextures load. Idempotent (0xffffffff-guarded).
	TerrainQuad::initMineTextureHandles();

	// Slice A — cement-overlay static-bake lifecycle. CPU-clear only; do NOT
	// build here. Mirrors the mine R7 timing-trap mitigation EXACTLY: the
	// overlay texture handles lazy-load in TerrainQuad::setupTextures during
	// the first paint cycle (before Render.TerrainOverlaysStatic fires), so
	// the build is deferred to the first armed DrawDecalStatic via
	// RebuildDecalStaticVBOIfDirty. ResetDecalStaticVBO leaves the dirty flag
	// set so that first armed draw bakes.
	gos_terrain_indirect::ResetDecalStaticVBO();
}

//---------------------------------------------------------------------------
long Terrain::worldToBlockIdx (float wx, float wy)
{
	// Verbatim transcription of GameObject::getBlockAndVertexNumber's block
	// math (gameobj.cpp). float2long truncation, the >>7 (==/128), and the
	// Y-flip are ALL load-bearing — do NOT "clean up" the float math.
	Assert(Terrain::worldUnitsPerVertex==128,0," block >>7 broken ");

	long mx = (float2long(wx) >> 7) + Terrain::halfVerticesMapSide;
	long blockX = float2long(mx * Terrain::oneOverVerticesBlockSide);

	long my = Terrain::halfVerticesMapSide - ((float2long(wy) >> 7) + 1);
	long blockY = float2long(my * Terrain::oneOverVerticesBlockSide);

	return blockX + (blockY * Terrain::blocksMapSide);
}

//---------------------------------------------------------------------------
void Terrain::purgeTransitions (void)
{
	terrainTextures->purgeTransitions();
	mapData->calcTransitions();
}

//---------------------------------------------------------------------------
void Terrain::destroy (void)
{
	// VPL-#shadow C-1 (CRITICAL): re-arm the one-shot full-map static
	// terrain shadow so the NEXT mission rebuilds it against fresh
	// blocks[]. Without this, the build-once latch stays set process-
	// lifetime and mission 2+ would project mission 1's frozen shadow
	// over mission 2's terrain (strictly worse than the original bug).
	// Must pair with the Phase-1 camera-accumulate retirement (same
	// commit). blocks[] is one-shot repopulated at next MapData::newInit.

	mc2terrain::TerrainFieldStore* fieldStore = gos_TerrainFieldStore();
	if (fieldStore) fieldStore->clearShoreFields();

	// Per-mission dense recipe teardown (Stage 2 indirect-terrain PR1).
	// Called from Mission::destroy → land->destroy() once per mission exit.
	// CPU-clears state; GL buffer is kept for reuse by next mission's Build.
	if (gos_terrain_indirect::IsEnabled() ||
	    gos_terrain_indirect::IsParityCheckEnabled()) {
		gos_terrain_indirect::ResetDenseRecipe();
	}
	// Phase 1: terrain lighting GPU compute shutdown (per-mission teardown).
	gos_terrain_lighting::mission_shutdown();

	// PR2c Stage 1c — mine static-bake teardown. CPU-clear; keep GL buffer
	// + texture-array allocations for next-mission reuse.
	gos_terrain_indirect::ResetMineStaticVBO();
	gos_terrain_indirect::ResetMineTextureArray();

	// Slice A — cement-overlay static-bake teardown. CPU-clear; keep the
	// GL_STATIC_DRAW buffer allocation for next-mission reuse (mirror
	// ResetMineStaticVBO teardown placement).
	gos_terrain_indirect::ResetDecalStaticVBO();

	// Terrain LOD chunk Phase 1 teardown — free before terrainHeap destroy.
	// MC2_TERRAIN_LOD_CHUNK=1 gate; idempotent (NULL guards prevent double-free).
	if (mc2TerrainLodChunkEnabled()) {
		if (s_blockMeta)      { std::free(s_blockMeta);      s_blockMeta      = nullptr; }
		if (s_superchunkMeta) { std::free(s_superchunkMeta); s_superchunkMeta = nullptr; }
		if (s_drawCmds)       { std::free(s_drawCmds);       s_drawCmds       = nullptr; }
		s_skirtDepths.reset();
		s_terrainChunkSide = 0;
		s_superchunkSide   = 0;
		s_cmdCount         = 0;
	}

	if (terrainTextures)
	{
		terrainTextures->destroy();
		terrainTexturesOwner.reset();
		terrainTextures = nullptr;
	}

	if (terrainTextures2)
	{
		terrainTextures2->destroy();
		terrainTextures2Owner.reset();
		terrainTextures2 = nullptr;
	}

	mapDataOwner.reset();
	mapData = nullptr;

	terrainNameOwner.reset();
	terrainName = nullptr;

	colorMapNameOwner.reset();
	colorMapName = nullptr;

	if (tileRowToWorldCoord)
	{
		std::free(tileRowToWorldCoord);
		tileRowToWorldCoord = nullptr;
	}

	if (tileColToWorldCoord)
	{
		std::free(tileColToWorldCoord);
		tileColToWorldCoord = nullptr;
	}

	if (cellToWorldCoord)
	{
		std::free(cellToWorldCoord);
		cellToWorldCoord = nullptr;
	}

	if (cellColToWorldCoord)
	{
		std::free(cellColToWorldCoord);
		cellColToWorldCoord = nullptr;
	}

	if (cellRowToWorldCoord)
	{
		std::free(cellRowToWorldCoord);
		cellRowToWorldCoord = nullptr;
	}

	if (moverBlockList)
	{
		std::free(moverBlockList);
		moverBlockList = nullptr;
	}

	if (usedBlockList)
	{
		std::free(usedBlockList);
		usedBlockList = nullptr;
	}

	if (vertexList)
	{
		std::free(vertexList);
		vertexList = nullptr;
	}

	if (quadList)
	{
		std::free(quadList);
		quadList = nullptr;
	}

	if (objBlockInfo)
	{
		std::free(objBlockInfo);
		objBlockInfo = nullptr;
	}
	
	if (objVertexActive)
	{
		std::free(objVertexActive);
		objVertexActive = nullptr;
	}
	

	
	numberVertices =
	numberQuads =
	
	halfVerticesMapSide = 
	realVerticesMapSide = 
		
	visibleVerticesPerSide =
	blocksMapSide = 0;
	
	worldUnitsMapSide = 0.0f;
	
	mapTopLeft3d = glm::vec3(0.0f);
		
	numObjBlocks = 0;

	recalcShadows = 
	recalcLight = false;

	//Reset these.  This will fix the mine problem.
	TerrainQuad::rainLightLevel = 1.0f;
	TerrainQuad::lighteningLevel = 0;
	TerrainQuad::mineTextureHandle = 0xffffffff;
	TerrainQuad::blownTextureHandle = 0xffffffff;
}

//---------------------------------------------------------------------------
void Terrain::setOverlayTile (long block, long vertex, long offset)
{
	mapData->setOverlayTile(block,vertex,offset);
}	

//---------------------------------------------------------------------------
void Terrain::setOverlay( long tileR, long tileC, Overlays type, DWORD offset )
{
	mapData->setOverlay( tileR, tileC, type, offset );
	// Slice A — public cement-overlay mutation chokepoint. Any caller that
	// changes a tile's overlay (bridge destroy routes through here via
	// Terrain::mapData->setOverlay at bldng.cpp, plus any future caller)
	// invalidates the mission-static decal bake. Mirrors MarkMineDirty at
	// the setMine chokepoint; idempotent (dirty-flag debounced).
	gos_terrain_indirect::MarkDecalDirty();
}

//---------------------------------------------------------------------------
void Terrain::setTerrain( long tileR, long tileC, int terrainType )
{
	mapData->setTerrain( tileR, tileC, terrainType );
}

//---------------------------------------------------------------------------
int Terrain::getTerrain( long tileR, long tileC )
{
	return mapData->getTerrain( tileR, tileC );
}

//---------------------------------------------------------------------------
void Terrain::calcWater (float waterDepth, float waterShallowDepth, float waterAlphaDepth)
{
	mapData->calcWater(waterDepth, waterShallowDepth, waterAlphaDepth);
}	

//---------------------------------------------------------------------------
long Terrain::getOverlayTile (long block, long vertex)
{
	return (mapData->getOverlayTile(block,vertex));
}	

//---------------------------------------------------------------------------
void Terrain::getOverlay( long tileR, long tileC, enum Overlays& type, DWORD& Offset )
{
	mapData->getOverlay( tileR, tileC, type, Offset );
}

//---------------------------------------------------------------------------
void Terrain::setVertexHeight( int VertexIndex, float Val )
{
	if ( VertexIndex > -1 && VertexIndex < realVerticesMapSide * realVerticesMapSide )
	{
		mapData->setVertexHeight( VertexIndex, Val );

		// Terrain LOD chunk Phase 3 — dirty-patch upload after edit.
		if (s_blockMeta && mc2TerrainLodChunkEnabled())
		{
			int vx = VertexIndex % static_cast<int>(realVerticesMapSide);
			int vy = VertexIndex / static_cast<int>(realVerticesMapSide);
			int bx = vx / static_cast<int>(verticesBlockSide);
			int by = vy / static_cast<int>(verticesBlockSide);
			if (bx >= s_terrainChunkSide) bx = s_terrainChunkSide - 1;
			if (by >= s_terrainChunkSide) by = s_terrainChunkSide - 1;
			int blockIdx = bx + by * s_terrainChunkSide;
			s_blockMeta[blockIdx].dirtyAabb = true;

			const TerrainBlockMeta& bm = s_blockMeta[blockIdx];
			int rows = bm.quadCountY + 1;
			int cols = bm.quadCountX + 1;
			std::vector<float> patch(static_cast<size_t>(rows) * cols);
			const PostcompVertex* blks = mapData->getBlocks();
			for (int dy = 0; dy < rows; ++dy)
				for (int dx = 0; dx < cols; ++dx)
					patch[dx + dy * cols] = blks[
						(bm.originX + dx) + (bm.originY + dy) * static_cast<int>(realVerticesMapSide)].elevation;
			gos_TerrainLodChunk_UploadHeightPatch(
				patch.data(), bm.originX, bm.originY,
				bm.quadCountX, bm.quadCountY, static_cast<int>(realVerticesMapSide));
		}
	}
}

//---------------------------------------------------------------------------
float Terrain::getVertexHeight( int VertexIndex )
{
	if ( VertexIndex > -1 && VertexIndex < realVerticesMapSide * realVerticesMapSide )
		return mapData->getVertexHeight(VertexIndex);

	return -1.f;
}

//---------------------------------------------------------------------------
float Terrain::getTerrainElevation (const glm::vec3 &position)
{
	float result = mapData->terrainElevation(position);
	return(result);
}

//---------------------------------------------------------------------------
float Terrain::getTerrainElevation( long tileR, long tileC )
{
	return mapData->terrainElevation( tileR, tileC );
}

//---------------------------------------------------------------------------
// getTerrainType: declared in terrain.h since forever but never implemented —
// no game code path called it, so the missing definition stayed latent. The
// editor pick bridge (EditorRenderBridge.cpp, EDITOR-OBJECTID-PICK-BRIDGE-1)
// is the first real caller, which surfaced it as an EditRel link error. Mirror
// the sibling tile accessors: world->tile via worldToTile, then the per-tile
// terrainType from MapData::getTerrain. getTerrain gosASSERTs its bounds, so
// guard OOB here and return -1 (the bridge's documented "unavailable" sentinel).
short Terrain::getTerrainType (const glm::vec3 &position)
{
	int tileR = 0, tileC = 0;
	worldToTile(position, tileR, tileC);
	if (tileR < 0 || tileC < 0 ||
		tileR >= Terrain::realVerticesMapSide ||
		tileC >= Terrain::realVerticesMapSide)
		return -1;
	return static_cast<short>(mapData->getTerrain(tileR, tileC));
}

//---------------------------------------------------------------------------
unsigned long Terrain::getTexture( long tileR, long tileC )
{
	return mapData->getTexture( tileR, tileC );
}

//---------------------------------------------------------------------------
float Terrain::getTerrainAngle (const glm::vec3 &position, glm::vec3* normal)
{
	float result = mapData->terrainAngle(position, normal);
	return(result);
}

//---------------------------------------------------------------------------
float Terrain::getTerrainLight (const glm::vec3 &position)
{
	float result = mapData->terrainLight(position);
	return(result);
}

//---------------------------------------------------------------------------
glm::vec3 Terrain::getTerrainNormal (const glm::vec3 &position)
{
	glm::vec3 result = Terrain::mapData->terrainNormal(position);
	return(result);
}

//---------------------------------------------------------------------------
void Terrain::setObjBlockActive (long blockNum, bool active)
{
	if ((blockNum >= 0) && (blockNum < numObjBlocks))
		objBlockInfo[blockNum].active = active;	
}	

//---------------------------------------------------------------------------
void Terrain::clearObjBlocksActive (void)
{
	for (long i = 0; i < numObjBlocks; i++)
		setObjBlockActive(i, false);
}	

//---------------------------------------------------------------------------
void Terrain::setObjVertexActive (long vertexNum, bool active)
{
	if ( (vertexNum >= 0) && (vertexNum < (realVerticesMapSide * realVerticesMapSide)) )
		objVertexActive[vertexNum] = active;	
}	

//---------------------------------------------------------------------------
void Terrain::clearObjVerticesActive (void)
{
	memset(objVertexActive,0,sizeof(bool) * realVerticesMapSide * realVerticesMapSide);
}

//---------------------------------------------------------------------------
long Terrain::save( PacketFile* fileName, int whichPacket, bool quickSave )
{ 
	if (!quickSave)
	{
		recalcShadows = true;
		mapData->calcLight();
	}
	else
	{
		recalcShadows = false;
	}
		
	return mapData->save( fileName, whichPacket ); 
}


//-----------------------------------------------------
bool Terrain::save( FitIniFile* fitFile )
{
	// write out the water info
#ifdef _DEBUG
	long result = 
#endif
	fitFile->writeBlock( "Water" );
	gosASSERT( result > 0 );


	fitFile->writeIdFloat( "Elevation", mapData->waterDepth );
	fitFile->writeIdFloat( "Frequency", waterFreq );
	fitFile->writeIdFloat( "Ampliture", waterAmplitude );
	fitFile->writeIdULong( "AlphaShallow", alphaEdge );
	fitFile->writeIdULong( "AlphaMiddle", alphaMiddle );
	fitFile->writeIdULong( "AlphaDeep", alphaDeep );
	fitFile->writeIdFloat( "AlphaDepth", mapData->alphaDepth );
	fitFile->writeIdFloat( "ShallowDepth", mapData->shallowDepth );

	fitFile->writeBlock( "Terrain" );
	fitFile->writeIdLong( "UserMin", userMin );
	fitFile->writeIdLong( "UserMax", userMax );
	fitFile->writeIdFloat( "TerrainMinX", tileColToWorldCoord[0] );
	fitFile->writeIdFloat( "TerrainMinY", tileRowToWorldCoord[0] );
	fitFile->writeIdUChar( "Noise", fractalNoise);
	fitFile->writeIdUChar( "Threshold", fractalThreshold);

	if (terrainTextures2)
	{
		terrainTextures2->saveTilingFactors(fitFile);
	}
	return true;
}

bool Terrain::load( FitIniFile* fitFile )
{
	// write out the water info
	long result = fitFile->seekBlock( "Water" );
	gosASSERT( result == NO_ERR );

	result = fitFile->readIdFloat( "Elevation", mapData->waterDepth );
	gosASSERT( result == NO_ERR );
	waterElevation = mapData->waterDepth;
	gos_SetWaterElevation(waterElevation);
	result = fitFile->readIdFloat( "Frequency", waterFreq );
	gosASSERT( result == NO_ERR );
	result = fitFile->readIdFloat( "Ampliture", waterAmplitude );
	gosASSERT( result == NO_ERR );
	result = fitFile->readIdULong( "AlphaShallow", alphaEdge );
	gosASSERT( result == NO_ERR );
	result = fitFile->readIdULong( "AlphaMiddle", alphaMiddle );
	gosASSERT( result == NO_ERR );
	result = fitFile->readIdULong( "AlphaDeep", alphaDeep );
	gosASSERT( result == NO_ERR );
	result = fitFile->readIdFloat( "AlphaDepth", mapData->alphaDepth );
	gosASSERT( result == NO_ERR );
	result = fitFile->readIdFloat( "ShallowDepth", mapData->shallowDepth );
	gosASSERT( result == NO_ERR );

	fitFile->seekBlock( "Terrain" );
	fitFile->readIdLong( "UserMin", userMin );
	fitFile->readIdLong( "UserMax", userMax );

	fitFile->readIdUChar( "Noise", fractalNoise);
	fitFile->readIdUChar( "Threshold", fractalThreshold);

	return true;

}

//---------------------------------------------------------------------------
void Terrain::unselectAll()
{
	mapData->unselectAll();
}

//---------------------------------------------------------------------------
void Terrain::selectVerticesInRect( const Stuff::Vector4D& topLeft, const Stuff::Vector4D& bottomRight, bool bToggle )
{
	glm::vec3 worldPos;
	Stuff::Vector4D screenPos;

	int xMin, xMax;
	int yMin, yMax;

	if ( topLeft.x < bottomRight.x )
	{
		xMin = topLeft.x;
		xMax = bottomRight.x;
	}
	else
	{
		xMin = bottomRight.x;
		xMax = topLeft.x;
	}

	if ( topLeft.y < bottomRight.y )
	{
		yMin = topLeft.y;
		yMax = bottomRight.y;
	}
	else
	{
		yMin = bottomRight.y;
		yMax = topLeft.y;
	}
	
	for ( int i = 0; i < realVerticesMapSide; ++i )
	{
		for ( int j = 0; j < realVerticesMapSide; ++j )
		{
			worldPos.y = tileRowToWorldCoord[j];
			worldPos.x = tileColToWorldCoord[i];
			worldPos.z = mapData->terrainElevation( j, i );

			// [PROJECTZ:SelectionPicking id=picking_terrain_rect_select]
			PROJECTZ_SITE("picking_terrain_rect_select", "SelectionPicking");
			eye->projectForSelectionPicking( worldPos, screenPos );

			if ( screenPos.x >= xMin && screenPos.x <= xMax &&
				 screenPos.y >= yMin && screenPos.y <= yMax )
			{
				mapData->selectVertex( j, i, true, bToggle );		
			}
		}
	}
}

//---------------------------------------------------------------------------
bool Terrain::hasSelection()
{
	return mapData->selection();
}

//---------------------------------------------------------------------------
bool Terrain::isVertexSelected( long tileR, long tileC )
{
	return mapData->isVertexSelected( tileR, tileC );
}

//---------------------------------------------------------------------------
bool Terrain::selectVertex( long tileR, long tileC, bool bSelect )
{
	//We never use the return value so just send back false.
	if ( (tileR <= -1) || (tileR >= realVerticesMapSide) )
		return false;

	if ( (tileC <= -1) || (tileC >= realVerticesMapSide) )
		return false;

	mapData->selectVertex( tileR, tileC, bSelect, 0 );
	return true;
}

//---------------------------------------------------------------------------
float Terrain::getHighestVertex( long& tileR, long& tileC )
{
	float highest = -9999999.; // an absurdly small number
	for ( int i = 0; i < realVerticesMapSide * realVerticesMapSide; ++i )
	{
		float tmp = getVertexHeight( i );
		if ( tmp > highest )
		{
			highest = tmp;
			tileR = i/realVerticesMapSide;
			tileC = i % realVerticesMapSide;
		}
	}

	return highest;
}

//---------------------------------------------------------------------------
float Terrain::getLowestVertex(  long& tileR, long& tileC )
{
	float lowest = 9999999.; // an absurdly big number
	for ( int i = 0; i < realVerticesMapSide * realVerticesMapSide; ++i )
	{
		float tmp = getVertexHeight( i );
		if ( tmp < lowest )
		{
			lowest = tmp;
			tileR = i/realVerticesMapSide;
			tileC = i % realVerticesMapSide;
		}
	}

	return lowest;
}

//---------------------------------------------------------------------------
void  Terrain::setUserSettings( long min, long max, int terrainType )
{
	userMin = min;
	userMax = max;
	baseTerrain = terrainType;
}

//---------------------------------------------------------------------------
void Terrain::getUserSettings( long& min, long& max, int& terrainType )
{
	min = userMin;
	max = userMax;
	terrainType = baseTerrain;
}

//---------------------------------------------------------------------------
void Terrain::recalcWater()
{
	mapData->recalcWater();
}

//---------------------------------------------------------------------------
// TERRAIN-MATERIAL-PAINT Slice 0 (BUG 1): re-run the load-time ttype[] build
// loop (terrain.cpp ~802-817) from blocks[].terrainType via terrainTypeToMaterial
// and re-upload the per-vertex type SSBO. The live LOD-chunk frag reads this SSBO
// every frame (v_terrainType), but it was uploaded only once at mission load and
// had no dirty/patch path, so painted material did not appear until reload. Call
// this at end-of-stroke from the material brush. Editor-lane additive helper.
void Terrain::refreshTerrainTypeSSBO()
{
	if (!mapData || !mapData->getBlocks())
		return;
	const int n = static_cast<int>(realVerticesMapSide) * static_cast<int>(realVerticesMapSide);
	if (n <= 0)
		return;
	std::vector<float> ttype(static_cast<size_t>(n));
	const PostcompVertex* blks = mapData->getBlocks();
	// MUST mirror the load-time mapping (terrain.cpp ttype[] loop) and the
	// duplicated terrainTypeToMaterial in quad.cpp / gos_terrain_indirect.cpp.
	auto terrainTypeToMaterial = [](DWORD t) -> float { return static_cast<float>(mc2terrain::terrainTypeToMaterial(static_cast<uint32_t>(t))); };
	for (int i = 0; i < n; ++i)
		ttype[i] = terrainTypeToMaterial(blks[i].terrainType);
	gos_TerrainLodChunk_UploadTerrainTypeFull(ttype.data(), static_cast<int>(realVerticesMapSide));
}

//---------------------------------------------------------------------------
void Terrain::reCalcLight(bool doShadows)
{
	recalcLight = true;
	recalcShadows = doShadows;
	
	//Do a new burnin for the colormap
	if (terrainTextures2)
	{
		if (colorMapName)
			terrainTextures2->recalcLight(colorMapName);
		else
			terrainTextures2->recalcLight(terrainName);
	}
}

//---------------------------------------------------------------------------
void Terrain::clearShadows()
{
	mapData->clearShadows();
}

//---------------------------------------------------------------------------

long Terrain::getWater (const glm::vec3& worldPos) {
	//-------------------------------------------------
	// Get elevation at this point and compare to deep
	// water altitude for this map.
	float elevation = getTerrainElevation(worldPos);
	
	if (elevation < (waterElevation - MapData::shallowDepth))
		return(2);
	if (elevation < waterElevation)
		return(1);
	return(0);
}
