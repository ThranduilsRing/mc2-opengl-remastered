# Phase split spec for Mission::update (code/mission_update.cpp). Line numbers are 1-based lines of the file at BASE.
BASE = '348ab2ee1'
SRC = 'code/mission_update.cpp'
FUNC_SIG = r'^long Mission::update \('
METHOD_CLASS = 'Mission'
INDENT = '\t'
DEDENT = ''
STRUCTS = {}
FRAME_GLOBAL = True
MEMBERS = []
EDITS = {}
ORCH_BEFORE = {}
ORCH_EXTRA = []
PARAMS = []

ORCH_DOC = ('// Per-frame mission update. The timing scopes, the active guard, the Tracy zone, the mission brain call (its 9999\n'
            '// result ends the update) and the LAB_ONLY timing sum stay here; each other phase is a member helper declared in\n'
            '// mission.h, run in the original order.')

D = {'dedent': '\t'}
PHASES = [
    ('updateDevHotkeys', 198, 293, 'Handle the developer hotkeys.', D),
    ('updateFrameClock', 295, 374, 'Advance the scenario clock from the fixed timestep or the wall clock and handle the in-mission save.', D),
    ('updateFrontEnd', 378, 420, 'Update the interface, camera, terrain, weather, waypoints and the path manager.', D),
    ('updateTerrainGeometry', 422, 430, 'Clear the terrain object flags and process the terrain geometry.', D),
    ('updateObjectManager', 432, 469, 'Begin the GPU cull frame and update the objects, or only their appearances while frozen.', D),
    ('updateCratersAndDamp', 471, 499, 'Update the craters and the mover visual-displacement damp.', D),
    ('updateSimSystems', 501, 510, 'Update the texture manager, the sensors and the collisions.', D),
    ('updateTraceHooks', 536, 563, 'Flush the ABL trace and emit the mover bridge, the identity hash and the coherence audit.', D),
    ('updateTermination', 565, 680, 'Decide whether the mission is over and start the termination countdown.', D),
    ('updateCheatsAndFrameRate', 682, 726, 'Handle the frame-rate toggle and the cheat keys and show the frame rate.', D),
    ('updateSoakAutoWin', 728, 748, 'Win the mission after the soak delay.', D),
    ('updateSoakKillEnemy', 750, 922, 'Run the soak kill, salvage and load-bay phases.', D),
    ('updateOrderLiveness', 924, 1049, 'Issue the order-liveness fixture orders and report their outcome.', D),
    ('updateSoakPilotPromote', 1051, 1119, 'Promote a deployed pilot on the soak missions.', D),
]
