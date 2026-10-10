# Phase split spec for GameCamera::render (code/gamecam.cpp). Line numbers are 1-based lines of the file at BASE.
BASE = 'b1462f214'
SRC = 'code/gamecam.cpp'
FUNC_SIG = r'^void GameCamera::render \(void\)'
METHOD_CLASS = 'GameCamera'
INDENT = '\t'
DEDENT = ''
STRUCTS = {}
FRAME_GLOBAL = True
MEMBERS = []
EDITS = {}
ORCH_BEFORE = {}
ORCH_EXTRA = []
PARAMS = []

ORCH_DOC = ('// Draws one camera frame. The viewport and light set-up, the clipper start, the scene pass skeleton with its Tracy zones\n'
            '// and lost-draw scopes stay here; the view-uniform publish, the diagnostics and the movie letterbox and fade quads\n'
            '// are member helpers declared in gamecam.h, run in the original order.')

P = {'dedent': '\t\t'}
PHASES = [
    ('renderViewUniforms', 305, 393, 'Fill the view uniforms, register the main scene view and compare them with the legacy MVP.', P),
    ('renderVanishProbe', 395, 441, 'Project sampled static-prop origins through the live camera for the vanish probe.', P),
    ('renderTerrainSceneState', 443, 497, 'Push the camera position, light direction, flat light, night state and daylight level to the terrain.', P),
    ('renderTacticalArcOverlay', 764, 824, 'Draw the range rings and firing arcs of the selected movers.', P),
    ('renderLetterbox', 839, 961, 'Draw the movie letterbox bars.'),
    ('renderFadeQuad', 963, 1024, 'Draw the movie fade-to-colour quad.'),
]
