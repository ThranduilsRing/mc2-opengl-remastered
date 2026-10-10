# Phase split spec for EditorInspector::drawImGui (GuiRuntime/EditorInspector.cpp). Line numbers are 1-based lines of the
# file at BASE.
BASE = 'db1a06216'
SRC = 'GuiRuntime/EditorInspector.cpp'
FUNC_SIG = r'^void EditorInspector::drawImGui\(\) \{'
INDENT = '    '
FILE_NAMESPACE = 'EditorInspector'
INTERNAL_INCLUDES = ['#include "EditorInspector_internal.h"']
SECT = 'GuiRuntime/EditorInspector_sections.cpp'
FILE_HEADS = {SECT: ['// EditorInspector: the Object Inspector section drawers drawImGui calls -- renderer features, copy-all, render explain,',
                      '// the per-kind panels, the pass sections and the material and gate tables. The orchestrator is in EditorInspector.cpp.']}
DEDENT = ''
STRUCTS = {}
FRAME_GLOBAL = True
MEMBERS = []
# kindName is a local of the orchestrator read once by the clipboard text; the helper asks pickKindName directly so no
# const char* parameter is needed.
EDITS = {421: ('"Kind: %s\\n", kindName);', '"Kind: %s\\n", pickKindName(s_selection.pickKind));')}
ORCH_BEFORE = {}
ORCH_EXTRA = []

# The orchestrator keeps the open / early-return logic, the window begin / end, the object-id header and the pick-kind
# chain skeleton; each collapsing section is a file-static helper that gets the lookup result when it reads it.
PARAMS = [('lk', 'const RenderWorld::LookupResult&')]

ORCH_DOC = ('// Draws the Object Inspector, the Renderer Features window and the Frame Inspector. The toggles, the early returns,\n'
            '// the window begin / end, the object-id header and the pick-kind chain stay here; each section body is a helper\n'
            '// above, run in the original order.')

N = {'dedent': '    ', 'file': SECT}
F = {'file': SECT}
PHASES = [
    ('drawFeaturesWindow', 235, 315, 'Draw the Renderer Features window.', F),
    ('drawCameraBookmarkSave', 377, 409, 'Draw the save-camera-bookmark and save-camera-view buttons.', F),
    ('drawCopyAll', 413, 480, 'Draw the Copy All button and copy the inspector text to the clipboard.', F),
    ('drawRenderExplain', 485, 694, 'Draw the render-explain section for a valid render-world pick.', F),
    ('drawLookup', 697, 705, 'Draw the lookup-result section.', F),
    ('drawStaticPropData', 709, 717, 'Draw the static-prop game data section.', N),
    ('drawRenderSpine', 720, 863, 'Draw the static-prop render spine section.', N),
    ('drawMechData', 865, 897, 'Draw the mech game data section.', N),
    ('drawMechDebugView', 905, 929, 'Draw the mech debug-view selector.', N),
    ('drawMechNormals', 938, 985, 'Draw the experimental mech normals controls.', N),
    ('drawMechLighting', 990, 1065, 'Draw the experimental mech lighting controls.', N),
    ('drawMechSnapshot', 1069, 1146, 'Draw the mech snapshot panel.', N),
    ('drawTerrainData', 1149, 1170, 'Draw the terrain pick data section.', N),
    ('drawTerrainPass', 1175, 1215, 'Draw the terrain pass section.', F),
    ('drawShadowPass', 1219, 1245, 'Draw the shadow pass section.', F),
    ('drawVfxPass', 1250, 1324, 'Draw the VFX pass section.', F),
    ('drawRenderPassContracts', 1329, 1353, 'Draw the render-pass contracts section.', F),
    ('drawMaterialInventory', 1361, 1417, 'Draw the static-prop material inventory section.', F),
    ('drawMaterialRwLookup', 1422, 1481, 'Draw the material handle lookup section.', F),
    ('drawEnvGates', 1484, 1513, 'Draw the environment gates section.', F),
]
