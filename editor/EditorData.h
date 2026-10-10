#ifndef EDITORDATA_H
#define EDITORDATA_H

#include <string>
#include "mclib.h"

#ifndef OBJECTIVE_H
#include "Objective.h"
#endif

#ifndef ELIST_H
#include "elist.h"
#endif

#ifndef ESTRING_H
#include "EString.h"
#endif

static const int GAME_MAX_PLAYERS = 8;

// S2.14: editor terrain camera-window side. Was 60, which clipped the visible
// terrain to a 60x60-vertex region around the camera on 100x100 maps (mc2_01),
// matching the user-reported "small region of terrain renders, props render
// at full world positions outside that region" symptom. The game uses
// GameVisibleVertices=200 (code/mission.cpp:251) which covers the largest
// stock map (200x200) edge-to-edge; the editor needs the same so the entire
// authored map is iterated by MapData::makeLists each frame regardless of
// camera position. Both CPU and GPU-indirect terrain paths consume this same
// camera-windowed vertexList/quadList, which is why MC2_TERRAIN_INDIRECT=0
// and MC2_TERRAIN_CULL_WIDE=1 did not expand the visible region (the upstream
// camera-window iterator at mapdata.cpp:1114-1117 caps both paths).
#define EDITOR_VISIBLE_VERTICES			500

namespace Microsoft
{
	namespace Xna
	{
		namespace Arm
		{
			struct IProviderAsset;
		}
	}
}

extern Microsoft::Xna::Arm::IProviderAsset * mapAsset;
extern Microsoft::Xna::Arm::IProviderAsset * mechAsset;

class CTeam
{
public:
	CTeam(int alignment = 0) { Alignment(alignment); }
	bool operator==(const CTeam &rhs) const;
	void Clear() { m_objectives.Clear(); }
	int Alignment() { return m_alignment; }
	void Alignment(int alignment) { m_alignment = alignment; m_objectives.Alignment(m_alignment); }
	bool Read( FitIniFile* missionFile ) { return m_objectives.Read(missionFile); }
	bool Save( FitIniFile* missionFile ) { return m_objectives.Save(missionFile); }
	CObjectives &ObjectivesRef() { return m_objectives; }
	void handleObjectInvalidation(const EditorObject *pObj) { m_objectives.handleObjectInvalidation(pObj); }
	bool NoteThePositionsOfObjectsReferenced() { return m_objectives.NoteThePositionsOfObjectsReferenced(); }
	bool RestoreObjectPointerReferencesFromNotedPositions() { return m_objectives.RestoreObjectPointerReferencesFromNotedPositions(); }
	bool ThereAreObjectivesWithNoConditions() { return m_objectives.ThereAreObjectivesWithNoConditions(); }
private:
	int m_alignment;
	CObjectives m_objectives;
};

class CTeams
{
public:
	CTeams();
	CTeams &operator=(const CTeams &master);
	bool operator==(const CTeams &rhs) const;
	void Clear();
	bool Read( FitIniFile* missionFile );
	bool Save( FitIniFile* missionFile );
	CTeam &TeamRef(int i);
	void handleObjectInvalidation(const EditorObject *pObj);
	bool NoteThePositionsOfObjectsReferenced();
	bool RestoreObjectPointerReferencesFromNotedPositions();
	bool ThereAreObjectivesWithNoConditions();
private:
	CTeam m_teamArray[GAME_MAX_PLAYERS];
};

class CPlayer
{
public:
	CPlayer() { Clear(); }
	void Clear() { m_defaultTeam = 0; }
	int DefaultTeam() { return m_defaultTeam; }
	void DefaultTeam(int team);
	bool Read( FitIniFile* missionFile, int playerNum );
	bool Save( FitIniFile* missionFile, int playerNum );
private:
	int m_defaultTeam;
};

class CPlayers
{
public:
	void Clear();
	CPlayer &PlayerRef(int i) { return m_playerArray[i]; }
	bool Read( FitIniFile* missionFile );
	bool Save( FitIniFile* missionFile );
private:
	CPlayers &operator=(const CPlayers &master) { /*not valid, do not use*/ }
	CPlayer m_playerArray[GAME_MAX_PLAYERS];
};

class MissionSettings
{
public:
	
	int largeArtillery;
	int smallArtillery;
	int cameraDrones;
	int sensors;
	int time;

	void clear(){ largeArtillery = smallArtillery = cameraDrones = sensors = time = 0; }
	MissionSettings(){ clear(); }

	bool save( FitIniFile* file );
};

class EditorData
{
	public:

		static EditorData* instance;

		EditorData();
		~EditorData();

		static bool clear();

		// ---------- Map-size helpers --------------------------------------------------
		// The three layers a map-size index maps to:
		//   cellSide   = quads per side (user-facing label).  Always a multiple of 20.
		//   vertexSide = cellSide + 1   (terrain vertex-array dimension).
		//   moveSide   = cellSide * 3   (MOVE pathfinding grid dimension).
		//                               moveSide % SECTOR_DIM(30) == 0 because
		//                               cellSide is a multiple of 20 and lcm(20,30)=60,
		//                               so every valid cellSide is also a multiple of 60.
		// Always use these helpers — never write raw switch/+1/*3 at call sites.
		static int MapSizeToCellSide(int sizeIndex);   // 0->60  1->80  ... 6->1020
		static int MapSizeToVertexSide(int sizeIndex); // cellSide + 1

		static bool initTerrainFromTGA( int mapSize, int min = 0, int max = 512, int terrain = 0 );
		// Generated mission (Path B): runs the python terrain generator for the
		// chosen size + terrain-type/biome + seed, then builds an editable map from
		// its heightmap + burnin colormap via initTerrainFromTGA.
		static bool generateMission( int mapSize, int terrain, unsigned long seed );
		// ImGui Map Generator dialog path: python has already run and written
		// terrain_gen_out/genmap.{burnin.tga,elev.r32}.  This function copies the
		// burnin to the textures path and applies the heightmap via initTerrainFromTGA.
		// mapSizeIndex matches MapSizeToCellSide() indices (0->60 cells..6->1020 cells).
		// biome is informational only (already baked into the generator output);
		// terrain index 0 is used so initTerrainFromTGA skips the colormap fallback.
		static bool generateFromDialogParams( int mapSizeIndex, const char* biome );
		// Amplify terrain relief: scale every vertex elevation about the mean by
		// `factor` (>1 exaggerates hills + deepens basins; keeps the heightmap shape).
		static bool amplifyTerrain( float factor );
		// Rebuild the terrain face cache + re-bake all GPU recipes after a terrain/
		// overlay edit. setTerrain/setOverlay NULL the face cache for the session;
		// without rebuilding, buildRecipeSlot gets nodeId=0 and the terrain renders
		// black. Call from terrain/overlay brush endPaint and undo/redo.
		static void refreshTerrainAfterEdit();
		static bool initTerrainFromPCV( const char* fileName );
		static bool	reassignHeightsFromTGA( const char* fileName, int min, int max );

		bool save( const char* fileName, bool quickSave = false );
		bool quickSave( const char* fileName );
		static bool saveHeightMap( File* file );
		bool saveMissionFitFileStuff( FitIniFile &fitFile );

		static void setMapName( const char* name );
		static const char* getMapName(){ return strlen( mapName ) ? mapName : 0; }

		// True when MOVE pathfinding data was loaded/built and is safe to write.
		// False for newly generated blank maps (set to true only after packet-4 read).
		static bool IsMoveDataReadyForFullSave();


		// Mark MOVE as dirty (needs rebuild).  Call after terrain generation or load
		// completes, BEFORE terrain/object state is stable enough to rebuild.
		// Does NOT rebuild immediately — safe to call from mid-load paths.
		static void MarkMoveDataDirty();

		// Build MOVE pathfinding in-memory from the current terrain + placed objects.
		// Returns false (with reason in *errorOut if provided) on failure.
		// Only call when land is fully initialized and object lists are stable.
		static bool RebuildMoveFromCurrentTerrain(std::string* errorOut = nullptr);

		// Rebuild only if dirty.  Returns true if MOVE is valid after the call
		// (either was clean, or rebuild succeeded).  Returns false only if a rebuild
		// was attempted and failed.  Safe to call every update() tick.
		static bool RebuildMoveIfDirty(std::string* errorOut = nullptr);
		static void updateTitleBar();

		EString MissionName() { return m_missionName; }
		void MissionName(EString missionName) { m_missionName = missionName; }

		bool MissionNameUseResourceString() { return m_missionNameUseResourceString; }
		void MissionNameUseResourceString(bool missionNameUseResourceString) { m_missionNameUseResourceString = missionNameUseResourceString; }

		int MissionNameResourceStringID() { return m_missionNameResourceStringID; }
		void MissionNameResourceStringID(int missionNameResourceStringID) { m_missionNameResourceStringID = missionNameResourceStringID; }

		EString Author() { return m_author; }
		void Author(EString author) { m_author = author; }

		EString Blurb() { return m_blurb; }
		void Blurb(EString blurb) { m_blurb = blurb; }

		bool BlurbUseResourceString() { return m_blurbUseResourceString; }
		void BlurbUseResourceString(bool blurbUseResourceString) { m_blurbUseResourceString = blurbUseResourceString; }

		int BlurbResourceStringID() { return m_blurbResourceStringID; }
		void BlurbResourceStringID(int blurbResourceStringID) { m_blurbResourceStringID = blurbResourceStringID; }

		EString Blurb2() { return m_blurb2; }
		void Blurb2(EString blurb2) { m_blurb2 = blurb2; }

		bool Blurb2UseResourceString() { return m_blurb2UseResourceString; }
		void Blurb2UseResourceString(bool blurb2UseResourceString) { m_blurb2UseResourceString = blurb2UseResourceString; }

		int Blurb2ResourceStringID() { return m_blurb2ResourceStringID; }
		void Blurb2ResourceStringID(int blurb2ResourceStringID) { m_blurb2ResourceStringID = blurb2ResourceStringID; }

		float TimeLimit() { return m_timeLimit; }
		void TimeLimit(float timeLimit) { m_timeLimit = timeLimit; }

		float DropWeightLimit() { return m_dropWeightLimit; }
		void DropWeightLimit(float dropWeightLimit) { m_dropWeightLimit = dropWeightLimit; }

		int InitialResourcePoints() { return m_initialResourcePoints; }
		void InitialResourcePoints(int initialResourcePoints) { m_initialResourcePoints = initialResourcePoints; }

		int CBills() { return m_CBills; }
		void CBills( int newCBills ) { m_CBills = newCBills; }

		bool IsSinglePlayer() { return m_isSinglePlayer; }
		void IsSinglePlayer(bool isSinglePlayer) { m_isSinglePlayer = isSinglePlayer; }

		int MaxTeams() { return m_maxTeams; }
		void MaxTeams(int maxTeams) { m_maxTeams = maxTeams; /* Not finished. Need to check 2 <= param <= 8, disable menu items, etc.,. */}

		int MaxPlayers() { return m_maxPlayers; }
		void MaxPlayers(int maxPlayers);

		CTeams &TeamsRef() { return m_teams; }
		CPlayers &PlayersRef() { return m_players; }

		int ScenarioTune() { return m_scenarioTune; }
		void ScenarioTune(int scenarioTune) { m_scenarioTune = scenarioTune; }

		EString VideoFilename() { return m_videoFilename; }
		void VideoFilename(EString videoFilename) { m_videoFilename = videoFilename; }

		int NumRandomRPbuildings() { return m_numRandomRPbuildings; }
		void NumRandomRPbuildings(int numRandomRPbuildings) { m_numRandomRPbuildings = numRandomRPbuildings; }

		EString DownloadURL() { return m_downloadURL; }
		void DownloadURL(EString downloadURL) { m_downloadURL = downloadURL; }

		int MissionType() { return m_missionType; }
		void MissionType(int missionType) { m_missionType = missionType; }

		bool AirStrikesEnabledDefault() { return m_airStrikesEnabledDefault; }
		void AirStrikesEnabledDefault(bool airStrikesEnabledDefault) { m_airStrikesEnabledDefault = airStrikesEnabledDefault; }

		bool MineLayersEnabledDefault() { return m_mineLayersEnabledDefault; }
		void MineLayersEnabledDefault(bool mineLayersEnabledDefault) { m_mineLayersEnabledDefault = mineLayersEnabledDefault; }

		bool ScoutCoptersEnabledDefault() { return m_scoutCoptersEnabledDefault; }
		void ScoutCoptersEnabledDefault(bool scoutCoptersEnabledDefault) { m_scoutCoptersEnabledDefault = scoutCoptersEnabledDefault; }

		bool SensorProbesEnabledDefault() { return m_sensorProbesEnabledDefault; }
		void SensorProbesEnabledDefault(bool sensorProbesEnabledDefault) { m_sensorProbesEnabledDefault = sensorProbesEnabledDefault; }

		bool UnlimitedAmmoEnabledDefault() { return m_unlimitedAmmoEnabledDefault; }
		void UnlimitedAmmoEnabledDefault(bool unlimitedAmmoEnabledDefault) { m_unlimitedAmmoEnabledDefault = unlimitedAmmoEnabledDefault; }

		bool AllTechEnabledDefault() { return m_allTechEnabledDefault; }
		void AllTechEnabledDefault(bool allTechEnabledDefault) { m_allTechEnabledDefault = allTechEnabledDefault; }

		bool RepairVehicleEnabledDefault() { return m_repairVehicleEnabledDefault; }
		void RepairVehicleEnabledDefault(bool repairVehicleEnabledDefault) { m_repairVehicleEnabledDefault = repairVehicleEnabledDefault; }

		bool SalvageCraftEnabledDefault() { return m_salvageCraftEnabledDefault; }
		void SalvageCraftEnabledDefault(bool salvageCraftEnabledDefault) { m_salvageCraftEnabledDefault = salvageCraftEnabledDefault; }

		bool ResourceBuildingsEnabledDefault() { return m_resourceBuildingsEnabledDefault; }
		void ResourceBuildingsEnabledDefault(bool resourceBuildingsEnabledDefault) { m_resourceBuildingsEnabledDefault = resourceBuildingsEnabledDefault; }

		bool NoVariantsEnabledDefault() { return m_noVariantsEnabledDefault; }
		void NoVariantsEnabledDefault(bool noVariantsEnabledDefault) { m_noVariantsEnabledDefault = noVariantsEnabledDefault; }

		bool ArtilleryPieceEnabledDefault() { return m_artilleryPieceEnabledDefault; }
		void ArtilleryPieceEnabledDefault(bool artilleryPieceEnabledDefault) { m_artilleryPieceEnabledDefault = artilleryPieceEnabledDefault; }

		bool RPsForMechsEnabledDefault() { return m_rPsForMechsEnabledDefault; }
		void RPsForMechsEnabledDefault(bool rPsForMechsEnabledDefault) { m_rPsForMechsEnabledDefault = rPsForMechsEnabledDefault; }

		int MaxRaindrops() { return m_maxRaindrops; }
		void MaxRaindrops(int maxRaindrops) { m_maxRaindrops = maxRaindrops; }

		float StartingRainLevel() { return m_startingRainLevel; }
		void StartingRainLevel(float startingRainLevel) { m_startingRainLevel = startingRainLevel; }

		float ChanceOfRain() { return m_chanceOfRain; }
		void ChanceOfRain(float chanceOfRain) { m_chanceOfRain = chanceOfRain; }

		float BaseLightningChance() { return m_baseLightningChance; }
		void BaseLightningChance(float baseLightningChance) { m_baseLightningChance = baseLightningChance; }
		
		long TheSkyNumber() 
		{
			return m_theSkyNumber;
		}
		
		void TheSkyNumber (long theSkyNumber)
		{
			m_theSkyNumber = theSkyNumber;
		}

		bool MissionNeedsSaving() { return m_missionNeedsSaving; }
		void MissionNeedsSaving(bool missionNeedsSaving) { m_missionNeedsSaving = missionNeedsSaving; }

		bool DetailTextureNeedsSaving() { return m_detailTextureNeedsSaving; }
		void DetailTextureNeedsSaving(bool detailTextureNeedsSaving) { m_detailTextureNeedsSaving = detailTextureNeedsSaving; }

		bool WaterTextureNeedsSaving() { return m_waterTextureNeedsSaving; }
		void WaterTextureNeedsSaving(bool waterTextureNeedsSaving) { m_waterTextureNeedsSaving = waterTextureNeedsSaving; }

		bool WaterDetailTextureNeedsSaving() { return m_waterDetailTextureNeedsSaving; }
		void WaterDetailTextureNeedsSaving(bool waterDetailTextureNeedsSaving) { m_waterDetailTextureNeedsSaving = waterDetailTextureNeedsSaving; }

		bool saveObjectives( FitIniFile* file );
		bool DoTeamDialog(int team) { return TeamsRef().TeamRef(team).ObjectivesRef().EditDialog(); }
		void handleObjectInvalidation(const EditorObject *pObj) { TeamsRef().handleObjectInvalidation(pObj); }

		void makeTacMap(BYTE*& pOutput, long& dataSize, int tacMapSize ); // this allocates memory, below just draws
		void drawTacMap( BYTE* pDest, long dataSize, int tacMapSize );
		void loadTacMap(PacketFile *file, BYTE*& pDest, long dataSize, int tacMapSize ); // this allocates memory.  Saves millions of CPU cycles on load 

	private:

		EditorData& operator=( const EditorData& editorData );
		EditorData( const EditorData& editorData );
		bool saveTacMap (PacketFile* file, long whichPacket );

		EString m_missionName;
		bool m_missionNameUseResourceString;
		int m_missionNameResourceStringID;
		EString m_author;
		EString m_blurb;
		bool m_blurbUseResourceString;
		int m_blurbResourceStringID;
		EString m_blurb2;
		bool m_blurb2UseResourceString;
		int m_blurb2ResourceStringID;
		float m_timeLimit;
		float m_dropWeightLimit;
		int m_initialResourcePoints;
		int	m_CBills;
		bool m_isSinglePlayer;
		int m_maxTeams;
		int m_maxPlayers;
		CTeams m_teams;
		CPlayers m_players;
		int m_scenarioTune;
		EString m_videoFilename;
		int m_numRandomRPbuildings;
		EString m_downloadURL;
		int m_missionType;

		bool m_airStrikesEnabledDefault;
		bool m_mineLayersEnabledDefault;
		bool m_scoutCoptersEnabledDefault;
		bool m_sensorProbesEnabledDefault;
		bool m_unlimitedAmmoEnabledDefault;
		bool m_allTechEnabledDefault;
		bool m_repairVehicleEnabledDefault;
		bool m_salvageCraftEnabledDefault;
		bool m_resourceBuildingsEnabledDefault;
		bool m_noVariantsEnabledDefault;
		bool m_artilleryPieceEnabledDefault;
		bool m_rPsForMechsEnabledDefault;

		static char mapName[256];

		static unsigned long*	tacMapBmp; // tac map data, keep around so we can update

		MissionSettings	missionSettings;

		int m_maxRaindrops;
		float m_startingRainLevel;
		float m_chanceOfRain;
		float m_baseLightningChance;
		
		long m_theSkyNumber;

		bool m_missionNeedsSaving;
		bool m_detailTextureNeedsSaving;
		bool m_waterTextureNeedsSaving;
		bool m_waterDetailTextureNeedsSaving;
};




#endif  // end of file ( EditorData.h )
