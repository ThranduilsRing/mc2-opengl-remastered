//---------------------------------------------------------------------------
//
// Terrain.h -- File contains class definitions for the terrain class.
//
//	MechCommander 2
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#ifndef TERRAIN_H
#define TERRAIN_H
//---------------------------------------------------------------------------
// Include Files
#ifndef MAPDATA_H
#include"mapdata.h"
#endif

#ifndef TERRTXM_H
#include"terrtxm.h"
#endif

#ifndef TERRTXM2_H
#include"terrtxm2.h"
#endif

#ifndef INIFILE_H
#include"inifile.h"
#endif

#ifndef MATHFUNC_H
#include"mathfunc.h"
#endif

#ifndef DQUAD_H
#include"dquad.h"
#endif

#ifndef DVERTEX_H
#include"dvertex.h"
#endif


//---------------------------------------------------------------------------
// Macro Definitions
#ifndef NO_ERR
#define NO_ERR		0
#endif

#define	MAPCELL_DIM				3
inline constexpr int MAX_MAP_CELL_WIDTH = 720;
inline constexpr float TACMAP_SIZE = 128.f;

// Per-cell flags for the editor passability overlay. Defined here so that
// mclib/quad.cpp can use them without depending on editor/ headers.
// Populated by EditorNavLayer (editor/EditorNavLayer.cpp) via gEditorNavFlags.
enum EditorNavFlags : uint8_t
{
    EDITOR_NAV_GROUND_PASSABLE = 1 << 0,
    EDITOR_NAV_HOVER_PASSABLE  = 1 << 1,
    EDITOR_NAV_AIR_PASSABLE    = 1 << 2,
    EDITOR_NAV_BLOCKED         = 1 << 3,
    EDITOR_NAV_SHALLOW_WATER   = 1 << 4,
    EDITOR_NAV_DEEP_WATER      = 1 << 5,
    EDITOR_NAV_OBJECT_BLOCKED  = 1 << 6,
};

extern uint8_t* gEditorNavFlags;      // null when overlay not active
extern int      gEditorNavCellSide;   // terrain cellSide = realVerticesMapSide - 1
extern bool     drawEditorPassability;

// Single source of truth for the terrain LOD chunk renderer gate. DEFAULT ON
// (cutover 2026-06-09); opt out with MC2_TERRAIN_LOD_CHUNK=0 (e.g. the editor, or
// to fall back to the legacy tessellated path). Cached on first call. Replaces the
// scattered getenv("MC2_TERRAIN_LOD_CHUNK") presence checks so they cannot drift.
bool mc2TerrainLodChunkEnabled();

//---------------------------------------------------------------------------
// Tactical mission-gated material profile (C1 — disposable).
// When the real material-palette architecture lands (post-Slice 0 design),
// this enum + the per-profile shader branches go away in one PR. Until
// then, mc2_24 sand renders are improved by routing low-saturation sand
// pixels into slot 2 (dirt) consistently — see notes in
// shaders/include/terrain_common.hglsl and shaders/gos_terrain.frag.
enum TerrainMaterialProfile {
	TERRAIN_MAT_PROFILE_LEGACY   = 0,
	TERRAIN_MAT_PROFILE_SAND_M24 = 1,
};
extern int g_terrainMaterialProfile;

//------------------------------------------------
// Put back in Move code when Glenn moves it over.
// 07/28/99 these numbers didn't correspond to clan/IS despite comment, so I chagned 'em.
// These MUST be these numbers or the game will not mark LOS correctly!
inline constexpr int NOTEAM = -1;
//#define TEAM1					0 	//this is PLAYER TEAM -- Single Player
inline constexpr int TEAM2 = 1;	//this is OPFOR TEAM -- Single Player
inline constexpr int TEAM3 = 2;	// this is allies 
inline constexpr int TEAM4 = 3;
inline constexpr int TEAM5 = 4;
inline constexpr int TEAM6 = 5;
inline constexpr int TEAM7 = 6;
inline constexpr int TEAM8 = 7;

//-------------------------------------------
// 08/01/99 -- Must have generic alignments or Heidi goes WAY south!
#define EDITOR_TEAMNONE			-1		//Allied
#define EDITOR_TEAM1			0		//Player
#define EDITOR_TEAM2			1		//Enemy
#define EDITOR_TEAM3			2
#define EDITOR_TEAM4			3
#define EDITOR_TEAM5			4
#define EDITOR_TEAM6			5
#define EDITOR_TEAM7			6
#define EDITOR_TEAM8			7

//---------------------------------------------------------------------------
// Used by the object system to load the objects on the terrain.
typedef struct _ObjBlockInfo
{
	bool		active;
	long		numCollidableObjects;
	long		numObjects;					// includes collidable objects
	long		firstHandle;				// collidables, followed by non
} ObjBlockInfo;

//---------------------------------------------------------------------------
// Terrain LOD chunk — Phase 1: per-block AABB + superchunk metadata.
// MC2_TERRAIN_LOD_CHUNK=1 gate. Zero behavior change when unset.

// Forward declaration; full type in GameOS/gameos/gos_terrain_lod_chunk.h.
struct TerrainDrawCommand;

struct TerrainBlockMeta {
	int            originX;     // blockX * 20 (vertex-grid index)
	int            originY;     // blockY * 20
	int            quadCountX;  // min(20, (realVerticesMapSide-1) - originX); 1021->always 20
	int            quadCountY;  // min(20, (realVerticesMapSide-1) - originY); 1021->always 20
	float          minElev;     // min elevation over [originX..originX+quadCountX] inclusive
	float          maxElev;     // max elevation over [originY..originY+quadCountY] inclusive
	bool           dirtyAabb;   // height changed -> recompute AABB + patch SSBO
	bool           inFrustum;   // result of AABB cull this frame
	unsigned char  lodLevel;    // 0-5, chosen each frame
	bool           hasConcrete; // Step 5c: any cement/concrete vertex -> clamp LOD fine
	                            // (cement word is per-tile; coarse LOD tears the runway)
};

struct SuperchunkMeta {
	float          worldMinX, worldMaxX;  // union AABB of constituent TerrainBlockMeta world extents
	float          worldMinY, worldMaxY;
	float          worldMinZ, worldMaxZ;
	bool           inFrustum;
	unsigned char  _pad[3];
};

//---------------------------------------------------------------------------
//Everything goes through here now.
// This will understand the original MC2 format and new format and will convert between
// Phase-helper state of Terrain::init (defined in terrain_init.cpp).
struct TerrainLodInitFrame;

class Terrain
{
	//Data Members
	//-------------
	protected:

		unsigned long							terrainHeapSize;
		
		long									numberVertices;
		long									numberQuads;
		VertexPtr								vertexList;
		TerrainQuadPtr							quadList;
		
	public:
		//For editor
		static long								userMin;
		static long								userMax;
		static unsigned long					baseTerrain;
		static unsigned char					fractalThreshold;
		static unsigned char					fractalNoise;

		static long								halfVerticesMapSide;		//Half of the below value.
		static long								realVerticesMapSide;		//Number of vertices on each side of map.
		
		static const long						verticesBlockSide;			//Always 20.
		static long								blocksMapSide;				//Calced from above and 
		static float							worldUnitsMapSide;			//Total world units map is across.
		static float							oneOverWorldUnitsMapSide;	//Inverse of the above.

		static long								visibleVerticesPerSide;		//How many should I process to be sure I got all I could see.

		static const float						worldUnitsPerVertex;		//How many world Units between each vertex.  128.0f in current universe.
		static const float						worldUnitsPerCell;			//How many world units between cells.  42.66666667f ALWAYS!!!!
		static const float						halfWorldUnitsPerCell;		//Above divided by two.
		static const float						metersPerCell;				//Number of meters per cell.  8.53333333f ALWAYS!!
		static const float						oneOverWorldUnitsPerVertex;	//Above numbers inverse.
		static const float						oneOverWorldUnitsPerCell;
		static const float						oneOverMetersPerCell;
		static const float						oneOverVerticesBlockSide;
		static const float						worldUnitsBlockSide;		//Total world units each block of 20 vertices is.  2560.0f in current universe.

		static glm::vec3					mapTopLeft3d;				//Where does the terrain start.
		
		static MapDataPtr						mapData;					//Pointer to class that manages terrain mesh data.
		static TerrainTexturesPtr				terrainTextures;			//Pointer to class that manages terrain textures.
		static TerrainColorMapPtr				terrainTextures2;			//Pointer to class that manages the NEW color map terrain texture.
		static UserHeapPtr						terrainHeap;				//Heap used for terrain.

		static char 							*terrainName;				//Name of terrain data file.
		static char								*colorMapName;				//Name of colormap, if different from terrainName.

		static float							oneOverWorldUnitsPerElevationLevel;

		static float							waterElevation;				//Actual height of water in world units.
		static float							frameAngle;					//Used to animate the waves
		static float							frameCos;
		static float							frameCosAlpha;
		static DWORD 							alphaMiddle;				//Used to alpha the water into the shore.
		static DWORD 							alphaEdge;
		static DWORD 							alphaDeep;
		static float							waterFreq;					//Used to animate waves.
		static float							waterAmplitude;

		static long		   						numObjBlocks;				//Stores terrain object info.
		static ObjBlockInfo						*objBlockInfo;				//Dynamically allocate this please!!
		
		static bool								*objVertexActive;			//Stores whether or not this vertices objects need to be updated

		static float 							*tileRowToWorldCoord;		//Arrays used to help change from tile and cell to actual world position.
		static float 							*tileColToWorldCoord;		//TILE functions will be obsolete with new system.
		static float 							*cellToWorldCoord;
		static float 							*cellColToWorldCoord;
		static float 							*cellRowToWorldCoord;

		static bool								recalcShadows;				//Should we recalc the shadow map!
		static bool								recalcLight;				//Should we recalc the light data.

		// Terrain LOD chunk Phase 1 — MC2_TERRAIN_LOD_CHUNK=1 gate only.
		static TerrainBlockMeta*				s_blockMeta;      // [s_terrainChunkSide * s_terrainChunkSide]
		static SuperchunkMeta*					s_superchunkMeta; // [s_superchunkSide * s_superchunkSide]
		static TerrainDrawCommand*				s_drawCmds;       // [s_terrainChunkSide * s_terrainChunkSide]
		static float*							s_skirtDepths;    // [s_terrainChunkSide * s_terrainChunkSide], parallel to s_drawCmds
		static int								s_cmdCount;
		static unsigned long					gCurrentFrame;    // starts at 1; incremented each frame
		static int								s_terrainChunkSide; // = ceil(quads/20); render chunk array dim
		static int								s_superchunkSide;   // = (s_terrainChunkSide + 3) / 4

	//Member Functions
	//-----------------
	public:

		void init (void);

		Terrain (void)
		{
			init();
		}

		void destroy (void);

		~Terrain (void)
		{
			destroy();
		}

		long init (PacketFile* file, int whichPacket, unsigned long visibleVertices, 
			volatile float& progress, float progressRange); // open an existing file
		long init( unsigned long verticesPerMapSide, PacketFile* file, unsigned long visibleVertices,
				volatile float& percent,
					float percentRange); // pass in null for a blank new map

		float getTerrainElevation (const glm::vec3 &position);
		short getTerrainType (const glm::vec3 &position);
		float getTerrainAngle (const glm::vec3 &position, glm::vec3* normal = NULL);
		glm::vec3 getTerrainNormal (const glm::vec3 &position);
		float getTerrainLight (const glm::vec3& position);
		bool isVisible (const glm::vec3 &looker, const glm::vec3 &looked_at);

		float getWaterElevation ()
		{
			return mapData->waterElevation();
		}


		long update (void);
		void render (void);
		// Terrain LOD chunk Phase 4: submit GPU draw commands built in update().
		// Called from code/gamecam.cpp after shadow pass, before renderLists().
		// No-op when MC2_TERRAIN_LOD_CHUNK is unset (s_blockMeta is nullptr).
		static void flushDrawCommands (void);
		void renderWater (void);
		// Stage 2 of renderWater architectural slice: GPU water fast path.
		// Called AFTER mcTextureManager->renderLists() so terrain has been
		// flushed and depth-written before water alpha-blends on top.
		// No-op when MC2_RENDER_WATER_FASTPATH is unset.
		void renderWaterFastPath (void);
		
		void geometry (void);
		void primeMissionTerrainCache (volatile float& progress, float progressRange);

		void drawTopView (void);

		static bool IsValidTerrainPosition (const glm::vec3 pos);
		static bool IsEditorSelectTerrainPosition (const glm::vec3 pos);
		static bool IsGameSelectTerrainPosition (const glm::vec3 pos);

		// FREE helper: world (raw MC2: x=east, y=north) -> terrain block index.
		// Replicates GameObject::getBlockAndVertexNumber's block math EXACTLY
		// (gameobj.cpp). Used ONLY by the two static-prop substrate-record
		// producers to populate GpuActorRecord::blockIdx for the C1b block
		// rollup. Does NOT compute vertexNum and is NOT a substitute for
		// getBlockAndVertexNumber (which has ~12 collision-critical callers).
		static long worldToBlockIdx (float wx, float wy);

		// Phase 7B: heightfield raycast for terrain picking.
		// Replaces the quadList AABB/screen-triangle scan inside
		// Camera::inverseProject when MC2_TERRAIN_LOD_CHUNK=1.
		// Ray is in MC2 world space (x=east, y=north, z=up).
		// Returns true and writes (outX,outY,outZ) on hit; false on miss.
		// Uses the full-resolution PostcompVertex heightfield, NOT LOD mesh.
		static bool raycastTerrain(
		    float ox, float oy, float oz,
		    float dx, float dy, float dz,
		    float* outX, float* outY, float* outZ);

		long save( PacketFile* fileName, int whichPacket, bool QuickSave = false);
		bool save( FitIniFile* fitFile ); // save stuff like water info
		bool load( FitIniFile* fitFile );

		// old overlay stuff
		void setOverlayTile (long block, long vertex, long offset);
		long getOverlayTile (long block, long vertex);
	
		// new overlay stuff
		void setOverlay( long tileR, long tileC, Overlays type, DWORD Offset );
		void getOverlay( long tileR, long tileC, Overlays& type, DWORD& Offset );
		void setTerrain( long tileR, long tileC, int terrainType );
		int	 getTerrain( long tileR, long tileC );
		unsigned long getTexture( long tileR, long tileC ); 
		float getTerrainElevation( long tileR, long tileC );

		void  setVertexHeight( int vertexIndex, float value ); 
		float getVertexHeight( int vertexIndex );

		void calcWater (float waterDepth, float waterShallowDepth, float waterAlphaDepth);

		void updateAllObjects (void);

		void setObjBlockActive (long blockNum, bool active);
		void clearObjBlocksActive (void);

		inline void worldToTile( const glm::vec3& pos, int& tileR, int& tileC );
		inline void worldToCell( const glm::vec3& pos, int& cellR, int& cellC );
		// Editor placement snap: round-to-nearest vertex (worldToCell floors -> up-left bias).
		// Game keeps worldToCell's floor for AI-grid stability.
		inline void worldToCellNearest( const glm::vec3& pos, int& cellR, int& cellC );
		inline void worldToTileCell (const glm::vec3& pos, int& tileR, int& tileC, int& cellR, int& cellC);
		inline void tileCellToWorld (int tileR, int tileC, int cellR, int cellC, glm::vec3& worldPos);
		inline void cellToWorld (int cellR, int cellC, glm::vec3& worldPos);

		inline void getCellPos( int cellR, int cellC,  glm::vec3& cellPos );
		
		void initMapCellArrays(void);

		void unselectAll();
		void selectVerticesInRect( const Stuff::Vector4D& topLeft, const Stuff::Vector4D& bottomRight, bool bToggle );
		bool hasSelection();
		bool isVertexSelected( long tileR, long tileC );
		bool selectVertex( long tileR, long tileC, bool bSelect = true );

		float getHighestVertex( long& tileR, long& tileC );
		float getLowestVertex(  long& tileR, long& tileC );

		static void setUserSettings( long min, long max, int terrainType );
		static void getUserSettings( long& min, long& max, int& terrainType );

		void recalcWater();
		// TERRAIN-MATERIAL-PAINT Slice 0 (BUG 1): re-upload the per-vertex
		// terrain-TYPE SSBO consumed by the live LOD-chunk frag so painted
		// material shows immediately (no mission reload). Mirrors the load-time
		// build+upload at terrain.cpp ttype[] loop. Additive editor-lane helper.
		void refreshTerrainTypeSSBO();
		void reCalcLight(bool doShadows = false);
		void clearShadows();

		long getWater (const glm::vec3& worldPos);

		float getClipRange()
		{
			return 0.5 * worldUnitsPerVertex * static_cast<float>(visibleVerticesPerSide);
		}

		void setClipRange(float clipRange)
		{
			visibleVerticesPerSide = 2.0 * clipRange / worldUnitsPerVertex;
		}
		
		void purgeTransitions (void);
		
		TerrainQuadPtr getQuadList (void)
		{
			return(quadList);
		}
		
		VertexPtr getVertexList (void)
		{
			return(vertexList);
		}
		
		long getNumVertices (void)
		{
			return(numberVertices);
		}
		
		long getNumQuads (void)
		{
			return(numberQuads);
		}
		
		void setObjVertexActive (long vertexNum, bool active);

		// Public read accessor for the cull active-set (Approach A: the slim
		// loop's dilated visible-cull superset).  Static because both
		// objVertexActive and realVerticesMapSide are class statics; bounds-
		// checked so an out-of-range vn is a clean false, never an OOB read.
		static bool getObjVertexActive (long vertexNum)
		{
			return (vertexNum >= 0 &&
			        vertexNum < (realVerticesMapSide * realVerticesMapSide))
			       ? objVertexActive[vertexNum] : false;
		}

		void clearObjVerticesActive (void);

		void resetVisibleVertices(long maxVisibleVertices);

		void getColorMapName (FitIniFile *file);
		void setColorMapName (char *mapName);
		void saveColorMapName (FitIniFile *file);

	// Terrain::init phases (terrain_init.cpp); callers use init().
	private:
		void initMapExtents(unsigned long verticesPerMapSide, unsigned long visibleVertices);
		void initTerrainHeap(volatile float& percent, float percentRange);
		void initTerrainTextures(PacketFile* pakFile, volatile float& percent, float percentRange);
		void initColorMapTexture(PacketFile* pakFile, volatile float& percent, float percentRange);
		void initObjBlockLists(volatile float& percent, float percentRange);
		void initMapData(PacketFile* pakFile, volatile float& percent, float percentRange);
		void lodAllocMeta();
		void lodHeightUpload(TerrainLodInitFrame& lf);
		void lodControlMap();
		void lodOverlayV2();
		void lodShorelineMask();
		void lodWaterShore();
		void lodVisualHeight(TerrainLodInitFrame& lf);
		void lodFlagConcrete(TerrainLodInitFrame& lf);
		void initVertexQuadLists(unsigned long visibleVertices);
		void initColorMapLoad();
		void initMaterialProfile();
};

typedef Terrain *TerrainPtr;

extern TerrainPtr land;

//---------------------------------------------------------------------------

inline void Terrain::worldToTile( const glm::vec3& pos, int& tileR, int& tileC )
{
	float tmpX = pos.x - land->mapTopLeft3d.x;
	float tmpY = land->mapTopLeft3d.y - pos.y;

	tileC = static_cast<int>(tmpX * oneOverWorldUnitsPerVertex);
	tileR =	static_cast<int>(tmpY * oneOverWorldUnitsPerVertex);
}

//---------------------------------------------------------------------------

inline void Terrain::worldToCell( const glm::vec3& pos, int& cellR, int& cellC )
{
	cellC = static_cast<int>(( pos.x - land->mapTopLeft3d.x ) * (oneOverWorldUnitsPerVertex*3.0f));
	cellR = static_cast<int>(( land->mapTopLeft3d.y - pos.y ) * (oneOverWorldUnitsPerVertex*3.0f));
}

//---------------------------------------------------------------------------

// Editor placement snap: round-to-nearest vertex (worldToCell floors -> up-left bias).
// Game keeps worldToCell's floor for AI-grid stability.
inline void Terrain::worldToCellNearest( const glm::vec3& pos, int& cellR, int& cellC )
{
	cellC = static_cast<int>(( pos.x - land->mapTopLeft3d.x ) * (oneOverWorldUnitsPerVertex*3.0f) + 0.5f);
	cellR = static_cast<int>(( land->mapTopLeft3d.y - pos.y ) * (oneOverWorldUnitsPerVertex*3.0f) + 0.5f);
}

//---------------------------------------------------------------------------

inline void Terrain::worldToTileCell( const glm::vec3& pos, int& tileR, int& tileC, int& cellR, int& cellC )
{
	float tmpX = pos.x - land->mapTopLeft3d.x;
	float tmpY = land->mapTopLeft3d.y - pos.y;

	tileC = tmpX * oneOverWorldUnitsPerVertex;
	tileR =	tmpY * oneOverWorldUnitsPerVertex;

	if ((tileC < 0) ||
		(tileR < 0) ||
		(tileC >= Terrain::realVerticesMapSide) ||
		(tileR >= Terrain::realVerticesMapSide))
	{
	#ifdef _DEBUG
		PAUSE(("called worldToTileCell with POS out of bounds? Result TC:%d TR:%d",tileC,tileR));
	#endif
		tileC = tileR = 0;
	}
		
	cellC = (pos.x - tileColToWorldCoord[tileC]) * oneOverWorldUnitsPerCell;
	cellR = (tileRowToWorldCoord[tileR] - pos.y) * oneOverWorldUnitsPerCell;
}

//---------------------------------------------------------------------------

inline void Terrain::tileCellToWorld (int tileR, int tileC, int cellR, int cellC, glm::vec3& worldPos) 
{
	if ((tileC < 0) ||
		(tileR < 0) ||
		(tileC >= Terrain::realVerticesMapSide) ||
		(tileR >= Terrain::realVerticesMapSide) ||
		(cellC < 0) ||
		(cellR < 0) ||
		(cellC >= MAPCELL_DIM) ||
		(cellR >= MAPCELL_DIM))
	{
	#ifdef _DEBUG
		PAUSE(("called cellToWorld with tile or cell out of bounds. TC:%d TR:%d CR:%d CC:%d",tileC,tileR,cellR,cellC));
	#endif
		tileR = tileC = cellR = cellC = 0;
	}
	else
	{
		worldPos.x = tileColToWorldCoord[tileC] + cellToWorldCoord[cellC] + halfWorldUnitsPerCell;
		worldPos.y = tileRowToWorldCoord[tileR] - cellToWorldCoord[cellR] - halfWorldUnitsPerCell;
		worldPos.z = 0.0f;
	}
}

//---------------------------------------------------------------------------

inline void Terrain::cellToWorld (int cellR, int cellC, glm::vec3& worldPos) 
{
	if ((cellR < 0) || 
		(cellC < 0) || 
		(cellR >= (Terrain::realVerticesMapSide * MAPCELL_DIM)) ||
		(cellC >= (Terrain::realVerticesMapSide * MAPCELL_DIM)))
	{
	#ifdef _DEBUG
		// sebi: !NB temporarily
		//PAUSE(("called cellToWorld with cell out of bounds. CellR:%d   CellC:%d",cellR,cellC));
	#endif
		worldPos.x = worldPos.y = worldPos.z = 0.0f;		
	}
	else
	{
		worldPos.x = cellColToWorldCoord[cellC] + halfWorldUnitsPerCell;
		worldPos.y = cellRowToWorldCoord[cellR] - halfWorldUnitsPerCell;
		worldPos.z = 0.0f;
	}
}

//---------------------------------------------------------------------------

inline void Terrain::getCellPos( int cellR, int cellC,  glm::vec3& cellPos )
{
	cellPos.x = (cellC * (worldUnitsPerVertex/3.)) + (worldUnitsPerVertex/6.);
	cellPos.y = (cellR * (worldUnitsPerVertex/3.)) + (worldUnitsPerVertex/6.);

	cellPos.x += land->mapTopLeft3d.x;
	cellPos.y = land->mapTopLeft3d.y - cellPos.y;

	cellPos.z = land->getTerrainElevation( cellPos );
}

//---------------------------------------------------------------------------
#endif

//---------------------------------------------------------------------------
//
// Edit Log
//
//---------------------------------------------------------------------------
