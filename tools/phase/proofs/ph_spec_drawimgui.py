# Phase split spec for EditorInspector::drawImGui (GuiRuntime/EditorInspector.cpp). Line numbers are 1-based lines of the
# file at BASE.
BASE = 'e61f403f1'
SRC = 'GuiRuntime/EditorInspector.cpp'
FUNC_SIG = r'^void EditorInspector::drawImGui\(\) \{'
INDENT = '    '
DEDENT = ''
STRUCTS = {}
FRAME_GLOBAL = True
MEMBERS = []
# kindName is a local of the orchestrator read once by the clipboard text; the helper asks pickKindName directly so no
# const char* parameter is needed.
EDITS = {522: ('"Kind: %s\\n", kindName);', '"Kind: %s\\n", pickKindName(s_selection.pickKind));')}
ORCH_BEFORE = {}
ORCH_EXTRA = []
PREFIX_EDITS = {323: 'using namespace EditorInspector;  // the drawImGui phases below name the namespace\'s types unqualified'}

# The orchestrator keeps the open / early-return logic, the window begin / end, the object-id header and the pick-kind
# chain skeleton; each collapsing section is a file-static helper that gets the lookup result when it reads it.
PARAMS = [('lk', 'const RenderWorld::LookupResult&')]

ORCH_DOC = ('// Draws the Object Inspector, the Renderer Features window and the Frame Inspector. The toggles, the early returns,\n'
            '// the window begin / end, the object-id header and the pick-kind chain stay here; each section body is a helper\n'
            '// above, run in the original order.')

N = {'dedent': '    '}
PHASES = [
    ('drawFeaturesWindow', 336, 416, 'Draw the Renderer Features window.'),
    ('drawCameraBookmarkSave', 478, 510, 'Draw the save-camera-bookmark and save-camera-view buttons.'),
    ('drawCopyAll', 514, 581, 'Draw the Copy All button and copy the inspector text to the clipboard.'),
    ('drawRenderExplain', 586, 795, 'Draw the render-explain section for a valid render-world pick.'),
    ('drawLookup', 798, 806, 'Draw the lookup-result section.'),
    ('drawStaticPropData', 810, 818, 'Draw the static-prop game data section.', N),
    ('drawRenderSpine', 821, 964, 'Draw the static-prop render spine section.', N),
    ('drawMechData', 966, 998, 'Draw the mech game data section.', N),
    ('drawMechDebugView', 1006, 1030, 'Draw the mech debug-view selector.', N),
    ('drawMechNormals', 1039, 1086, 'Draw the experimental mech normals controls.', N),
    ('drawMechLighting', 1091, 1166, 'Draw the experimental mech lighting controls.', N),
    ('drawMechSnapshot', 1170, 1247, 'Draw the mech snapshot panel.', N),
    ('drawTerrainData', 1250, 1271, 'Draw the terrain pick data section.', N),
    ('drawTerrainPass', 1276, 1316, 'Draw the terrain pass section.'),
    ('drawShadowPass', 1320, 1346, 'Draw the shadow pass section.'),
    ('drawVfxPass', 1351, 1425, 'Draw the VFX pass section.'),
    ('drawRenderPassContracts', 1430, 1454, 'Draw the render-pass contracts section.'),
    ('drawMaterialInventory', 1462, 1518, 'Draw the static-prop material inventory section.'),
    ('drawMaterialRwLookup', 1523, 1582, 'Draw the material handle lookup section.'),
    ('drawEnvGates', 1585, 1614, 'Draw the environment gates section.'),
]
