# Phase split spec for GameObjectManager::update (code/objmgr_update.cpp). Line numbers are 1-based lines of the file at BASE.
BASE = '7494b8738'
SRC = 'code/objmgr_update.cpp'
FUNC_SIG = r'^void GameObjectManager::update \('
METHOD_CLASS = 'GameObjectManager'
INDENT = '\t'
DEDENT = ''
STRUCTS = {}

# Locals the orchestrator keeps (they carry RAII spans / statics) and hands to the helpers that name them.
PARAMS = [
    ('frameJobsPrePassCount', 'int&'),
    ('removeList', 'MoverPtr (&removeList)[MAX_MOVERS]'),
    ('numRemoved', 'long&'),
]
MEMBERS = []
EDITS = {}
ORCH_BEFORE = {}
FRAME_DOC = ''
ORCH_DOC = ('// One simulation tick of every owned object. The RAII cost spans and Tracy zones stay here so each still\n'
            '// brackets the same phases; each phase body is a member helper declared in objmgr.h, in execution order.')

T1 = {'dedent': '\t'}
PHASES = [
    ('updateDestroyTestHook', 422, 444, 'Test-only hook: destroy the first live building at a configured frame.'),
    ('updateReadbackSnapshot', 449, 459, 'Build the per-actor GPU visibility snapshot before the sweep reads it.'),
    ('updateActiveSweep', 461, 547, 'Single sweep over objList that maintains framesSinceActive and the conflation probe.'),
    ('updateFrameJobsPrepass', 571, 719, 'Parallel recalcBounds / touch pre-pass for the terrain objects.', T1),
    ('updateTerrainObjects', 723, 1266, 'Update special buildings, gates and the terrain object blocks, then report the counters.', T1),
    ('updateMoverWalk', 1274, 1318, 'Update every mech and vehicle and queue removed movers.', T1),
    ('updateBrainCommit', 1324, 1367, 'Deferred brain-intent commit pass in warrior-ID order.', T1),
    ('updateOtherFx', 1372, 1436, 'Update turrets, weapons, carnage, lights and artillery.', T1),
]
