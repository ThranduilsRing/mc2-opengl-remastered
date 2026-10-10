# Phase split spec for Mission::init (code/mission_init.cpp). Line numbers are 1-based lines of the file at BASE.
BASE = 'e622fd627'
SRC = 'code/mission_init.cpp'
FUNC_SIG = r'^void Mission::init \('
METHOD_CLASS = 'Mission'
INDENT = '\t'
DEDENT = ''
STRUCTS = {'f': 'MissionInitFrame'}
ALIAS_PER_LINE = 3
FRAME_HEADER = 'code/mission_init_frame.h'
FRAME_HEADER_DOC = 'Locals Mission::init hands from one load phase to the next.'
SPLIT_AT = 'initWarriorsHeader'
SPLIT_SRC = 'code/mission_init_load.cpp'
SPLIT_HEAD = ['// Mission: the second half of the Mission::init load sequence -- warriors and brains, movers and parts,',
              '// terrain objects, commanders, the HUD and the geometry finalize. The first half and the orchestrator are in mission_init.cpp.']

PARAMS = [
    ('loadType', 'long'),
    ('dropZoneID', 'long'),
    ('dropZoneList', 'Stuff::Vector3D*'),
    ('commandersToLoad', 'char (*commandersToLoad)[3]'),
    ('numMoversPerCommander', 'long'),
]

# Locals that live across the load phases. The statement that declared each stays in its phase as an assignment.
MEMBERS = [
    ('missionName', 'const char*', 'f', [0]),
    ('gameSystemFile', 'FitIniFile*', 'f', [157]),
    ('result', 'long', 'f', [160]),
    ('forestMoveCost', 'long', 'f', [241]),
    ('missionFileName', 'FullPathFileName', 'f', [393]),
    ('maxTeamID', 'long', 'f', [502]),
    ('maxCommanderID', 'long', 'f', [503]),
    ('pakFile', 'PacketFile', 'f', [724]),
    ('numErrors', 'long', 'f', [0]),
    ('numLinesProcessed', 'long', 'f', [0]),
    ('numWarriors', 'unsigned long', 'f', [942]),
    ('loadBrainParameters', 'bool', 'f', [955]),
    ('soakBrainsLoaded', 'long', 'f', [958]),
    ('soakBrainsFailed', 'long', 'f', [959]),
]

EDITS = {
    837: ('long numErrors = 0, numLinesProcessed = 0;', 'numErrors = 0, numLinesProcessed = 0;'),
}

# The frame carries the mission name so no helper declares a const char* parameter (cxx17 LG06).
ORCH_BEFORE = {39: '\tMissionInitFrame f;\n\tf.missionName = missionName;'}
ORCH_EXTRA = ['f.missionName = missionName;']

FRAME_DOC = '// The mission-file handles, parse results and counters the Mission::init phases share.'

ORCH_DOC = ('// Mission load sequence. Each phase is a member helper declared in mission.h, run in the original order; the\n'
            '// shared locals live in MissionInitFrame. The Tracy zone and the missionFileName char-array reset stay here.')

PHASES = [
    ('initPrologue', 39, 91, 'Reset the per-mission sim state, reseed the RNG and clear the termination flags.'),
    ('initTeamRelations', 93, 131, 'Load the SP or MP team relation matrix and reset the turn counters.'),
    ('initCoreSystems', 133, 152, 'Load ABL, the TGL and the mission heap.'),
    ('initGameSystemRanges', 154, 248, 'Open gamesys.fit and read the weapon ranges and the general visual / sensor settings.'),
    ('initGameSystemGeneral', 250, 324, 'Read the component list, the god-mode / footprint flags and the mover game-system blocks.'),
    ('initGameSystemHazards', 326, 388, 'Read the mine, smoke and fire settings.'),
    ('initMissionFile', 393, 416, 'Open the mission .fit and activate the mod for it.'),
    ('initSkyAndTeamReset', 418, 500, 'Read the sky number, reset stale teams and commanders and run the cycle-test fixture.'),
    ('initTeamsAndCommanders', 502, 648, 'Create the teams and commanders from the mission file or the multiplayer setup.'),
    ('initTriggerAndDropZone', 650, 677, 'Create the trigger-area and sensor managers and read the drop zone.'),
    ('initMissionSettings', 679, 718, 'Read the mission settings, create the crater and object managers.'),
    ('initTerrain', 721, 768, 'Open the terrain pak and load, prime and size the terrain.'),
    ('initPathfinding', 771, 835, 'Initialise the move system, the path-find maps and the path manager.'),
    ('initAblCoreLibs', 837, 869, 'Mount the ABL core libraries.'),
    ('initMissionScript', 871, 931, 'Pick the mission script and build the mission brain.'),
    ('initWarriorsHeader', 933, 959, 'Read the warrior count and set up the brain-load counters.'),
    ('loadWarriors', 960, 1056, 'Load each warrior and its brain.'),
    ('soakAutowinCheck', 1057, 1062, 'Arm the soak auto-win when requested or when a brain failed to load.'),
    ('loadBrainParamsPhase', 1064, 1084, 'Load the per-warrior brain parameters.'),
    ('loadMissionFitOpord', 1086, 1231, 'Load the mission-fit order sets for the enhanced brains.'),
    ('loadBrainRuntime', 1233, 1288, 'Load the brain runtime tables.'),
    ('loadTacticWeights', 1290, 1351, 'Load the tactic weight tables.'),
    ('loadBrainDispatch', 1353, 1481, 'Resolve which brain dispatch mode each warrior runs.'),
    ('initPartsHeader', 1483, 1512, 'Read the parts block header.'),
    ('initPartsLoad', 1514, 1790, 'Create every mover and part named by the mission file.'),
    ('initObjectCounts', 1792, 1832, 'Count the mechs, vehicles and terrain objects and size the object manager.'),
    ('initGpuAndLighting', 1834, 1873, 'Initialise GPU culling, the frame-job pool, terrain lighting and shadow priming.'),
    ('initSpawnActors', 1875, 1923, 'Spawn the movers and vehicles for each commander.'),
    ('initTerrainObjectsLoad', 1925, 1938, 'Load the terrain objects and build the mover lists.'),
    ('initDisableAndObjectives', 1940, 1990, 'Apply the start-of-mission disables and load the team objectives.'),
    ('initCommanders', 1992, 2062, 'Finish the commanders, rosters and mission clocks.'),
    ('initWeatherCamera', 2064, 2085, 'Close gamesys.fit and create the weather and the camera.'),
    ('initInterface', 2087, 2126, 'Create the mission interface, load its screen and finish the pilot log.'),
    ('initLabTimingAndClose', 2128, 2151, 'Scale the lab timing counters and close the mission file.'),
    ('initFinalizeGeometry', 2153, 2183, 'Finalise the static-prop and mech geometry and the GPU cull buffers.'),
]
