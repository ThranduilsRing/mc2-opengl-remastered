# Phase split spec for Mission::load (code/saveload.cpp). Line numbers are 1-based lines of the file at BASE.
BASE = 'b6917fc91'
SRC = 'code/saveload.cpp'
FUNC_SIG = r'^void Mission::load \('
METHOD_CLASS = 'Mission'
INDENT = '\t'
DEDENT = ''
STRUCTS = {'f': 'MissionLoadFrame'}
ALIAS_PER_LINE = 3
FRAME_HEADER = 'code/mission_load_frame.h'
FRAME_HEADER_DOC = 'Locals Mission::load hands from one load phase to the next.'
INTERNAL_INCLUDES = ['#include "saveload_internal.h"']
SPLIT_AT = 'loadAblAndManagers'
SPLIT_SRC = 'code/saveload_load.cpp'
SPLIT_HEAD = ['// Mission: the second half of the Mission::load sequence -- the managers, terrain, movement data and mission script,',
              '// the warriors, parts, teams and objects, the commanders, the HUD and the geometry finalize. The first half and the',
              '// orchestrator are in saveload.cpp.']

PARAMS = []

# Locals that live across the load phases. The statement that declared each stays in its phase as an assignment.
MEMBERS = [
    ('loadFileName', 'const char*', 'f', [0]),
    ('oldXLoc', 'DWORD', 'f', [454]),
    ('oldYLoc', 'DWORD', 'f', [455]),
    ('turnOffAsyncMouse', 'bool', 'f', [460]),
    ('loadFile', 'PacketFile', 'f', [467]),
    ('result', 'long', 'f', [468]),
    ('currentPacket', 'DWORD', 'f', [472]),
    ('tmpString[2048]', 'char', 'f', [544]),
    ('gameSystemFile', 'FitIniFile*', 'f', [575]),
    ('forestMoveCost', 'long', 'f', [638]),
    ('tmpName', 'FullPathFileName', 'f', [760]),
    ('missionFile', 'FitIniFile', 'f', [778]),
    ('rps', 'long', 'f', [797]),
    ('pakFile', 'PacketFile', 'f', [941]),
]

EDITS = {}

# The frame carries the save-file name so no helper declares a const char* parameter (cxx17 LG06).
ORCH_BEFORE = {446: '\tMissionLoadFrame f;\n\tf.loadFileName = loadFileName;'}
ORCH_EXTRA = ['f.loadFileName = loadFileName;']

FRAME_DOC = '// The save-file handle, the unpacked mission file and the counters the Mission::load phases share.'

ORCH_DOC = ('// In-mission save restore. Each phase is a member helper declared in mission.h, run in the original order, so the\n'
            '// packets are read from the save file in the order Mission::save wrote them; the shared locals live in\n'
            '// MissionLoadFrame.')

PHASES = [
    ('loadPrologue', 446, 464, 'Trace the load, silence the mouse and start the loading-screen timer.'),
    ('openSaveFile', 466, 506, 'Open the save packet file and reject a missing file or a wrong version stamp.', {'exits': True}),
    ('loadDestroyAndRestore', 508, 542, 'Tear down the running mission and reset the mech and static-prop batchers.'),
    ('loadResetSimState', 544, 561, 'Reset the turn counters and bring up ABL, the TGL and the mission heap.'),
    ('loadGameSystemRanges', 563, 640, 'Open gamesys.fit and read the weapon ranges and the general visual / sensor settings.'),
    ('loadGameSystemGeneral', 642, 701, 'Read the component list, the god-mode / footprint flags and the mover game-system blocks.'),
    ('loadGameSystemHazards', 703, 751, 'Read the mine, smoke and fire settings.'),
    ('loadMissionSettings', 753, 872, 'Unpack the saved mission .fti and read the mission settings, the sky and the script name.'),
    ('loadAblAndManagers', 874, 934, 'Create the trigger-area, crater and object managers.'),
    ('loadTerrain', 936, 979, 'Open the terrain pak, load the terrain and re-arm the terrain GPU state.'),
    ('loadMovement', 981, 1012, 'Read the saved move maps and create the path manager.'),
    ('loadMissionScript', 1014, 1064, 'Restore the mission ABL environment and find the mission brain.'),
    ('loadWarriorsAndPartsHeader', 1066, 1093, 'Load the saved warriors and read the parts block header.'),
    ('loadParts', 1095, 1206, 'Read every part named by the saved mission file.'),
    ('loadTeamsAndObjects', 1208, 1273, 'Load the teams, the sensor manager and the objects, then re-arm the GPU cull buffers.'),
    ('loadObjectivesCommanders', 1275, 1326, 'Read the objectives and the commanders and close gamesys.fit.'),
    ('loadWeatherCamera', 1328, 1344, 'Create the weather and the camera.'),
    ('loadInterface', 1346, 1387, 'Create the mission interface and finish the pilot log.'),
    ('loadLogistics', 1389, 1431, 'Close the mission file and restore the logistics data and the current mission number.'),
    ('loadActivateAndFinalize', 1433, 1471, 'Activate the camera and finalise the static-prop and mech geometry and the GPU cull buffers.'),
    ('loadRestoreInput', 1473, 1497, 'Stop the loading-screen timer and restore the mouse and the frame clock.'),
]
