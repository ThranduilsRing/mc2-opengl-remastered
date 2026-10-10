# Phase split spec for MissionBegin::update (code/missionbegin.cpp). Line numbers are 1-based lines of the file at BASE.
BASE = '8b6f07bf2'
SRC = 'code/missionbegin.cpp'
FUNC_SIG = r'^const char\* MissionBegin::update\(\)'
METHOD_CLASS = 'MissionBegin'
INDENT = '\t'
DEDENT = ''
STRUCTS = {}
FRAME_GLOBAL = True
MEMBERS = []
EDITS = {}
ORCH_BEFORE = {}
ORCH_EXTRA = []

# The orchestrator keeps the screen lookup, the logistics brain call, the screen update, the soak gate with its
# force-next flag and the bReadyToLoad latch; each phase gets the screen and the flag it names.
PARAMS = [
    ('pCurScreen', 'LogisticsScreen*'),
    ('soakForceNext', 'bool&'),
]

ORCH_DOC = ('// Per-frame front-end update. The screen lookup, the logistics brain call, the screen update, the soak gate and the\n'
            '// ready-to-load latch stay here; each other phase is a member helper declared in missionbegin.h, run in the\n'
            '// original order. A phase that leaves the update returns true and the orchestrator returns NULL.')

T = {'dedent': '\t'}
PHASES = [
    ('updateDumpFrontEndState', 720, 745, 'Dump the front-end state to the LOGISTICS diagnostic event when the screen changes.'),
    ('updateSplash', 747, 860, 'Run the main menu and leave it for the first screen or a restart.', {'exits': 'NULL', 'noparams': ['pCurScreen']}),
    ('updateTutorialAnimation', 874, 1001, 'Move the mouse and flash the target of the running tutorial callout.', T),
    ('updateSoakAutoPurchase', 1019, 1291, 'Buy and deploy a mech so a purchase-required mission can launch under auto-advance.', T),
    ('updateSoakScreenWalk', 1293, 1547, 'Walk the settled screen toward launch, through the optional screen-check detour.', T),
    ('updateScreenAdvance', 1569, 1687, 'End the finished screen and begin the next or the previous one.', T),
]
