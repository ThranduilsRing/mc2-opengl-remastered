# Phase split spec for TG_Shape::MultiTransformShape (mclib/tgl_transform.cpp). Line numbers are 1-based lines of the
# file at BASE.
BASE = 'a9273e951'
SRC = 'mclib/tgl_transform.cpp'
FUNC_SIG = r'^long TG_Shape::MultiTransformShape \('
METHOD_CLASS = 'TG_Shape'
INDENT = '\t'
DEDENT = ''
STRUCTS = {'f': 'TgXformFrame'}
ALIAS_PER_LINE = 3
FRAME_HEADER = 'mclib/tgl_transform_frame.h'
FRAME_HEADER_DOC = 'Per-vertex and per-face colour accumulators the MultiTransformShape lighting phases share.'
INTERNAL_INCLUDES = ['#include "tgl_internal.h"']

# The orchestrator keeps the RAII timing scopes, the pool allocation, the two loop headers with their per-iteration
# frame, the face cull test and the return value; every phase gets the orchestrator locals and parameters it names.
PARAMS = [
    ('shapeToClip', 'Stuff::Matrix4D*'),
    ('parentNode', 'TG_ShapeRecPtr'),
    ('isHudElement', 'bool'),
    ('alphaValue', 'BYTE'),
    ('isClamped', 'bool'),
    ('oneOff', 'bool&'),
    ('oneOn', 'bool&'),
    ('isNight', 'bool'),
    ('nightFactor', 'float'),
    ('theShape', 'TG_TypeShapePtr'),
    ('j', 'long'),
]

MEMBERS = [
    ('redFinal', 'DWORD', 'f', [0]),
    ('greenFinal', 'DWORD', 'f', [0]),
    ('blueFinal', 'DWORD', 'f', [0]),
    ('redAmb', 'DWORD', 'f', [0]),
    ('greenAmb', 'DWORD', 'f', [0]),
    ('blueAmb', 'DWORD', 'f', [0]),
    ('redSpec', 'DWORD', 'f', [0]),
    ('greenSpec', 'DWORD', 'f', [0]),
    ('blueSpec', 'DWORD', 'f', [0]),
    ('startVLight', 'DWORD', 'f', [302]),
    ('doHotGreenTrace', 'bool', 'f', [0]),
]

EDITS = {
    293: ('DWORD redFinal=0, greenFinal=0, blueFinal=0;', 'redFinal=0, greenFinal=0, blueFinal=0;'),
    294: ('DWORD redAmb = 0, greenAmb = 0, blueAmb = 0;', 'redAmb = 0, greenAmb = 0, blueAmb = 0;'),
    295: ('DWORD redSpec=0, greenSpec=0, blueSpec=0;', 'redSpec=0, greenSpec=0, blueSpec=0;'),
    417: ('bool doHotGreenTrace =', 'doHotGreenTrace ='),
    864: ('DWORD redFinal = 0, greenFinal = 0, blueFinal = 0;', 'redFinal = 0, greenFinal = 0, blueFinal = 0;'),
    865: ('DWORD redSpec=0, greenSpec=0, blueSpec=0;', 'redSpec=0, greenSpec=0, blueSpec=0;'),
    866: ('DWORD redAmb=0, greenAmb=0, blueAmb=0;', 'redAmb=0, greenAmb=0, blueAmb=0;'),
}

ORCH_BEFORE = {293: '\t\tTgXformFrame f;', 864: '\t\t\tTgXformFrame f;'}
ORCH_EXTRA = []

FRAME_DOC = '// The colour accumulators one vertex or one face builds across the lighting phases.'

ORCH_DOC = ('// Transforms, lights and queues one shape. The timing scopes, the pool allocation, the per-vertex and per-face loop\n'
            '// headers with their colour frame, the face cull test and the result stay here; each phase body is a member\n'
            '// helper declared in tgl.h.')

LIGHT = 'mclib/tgl_transform_light.cpp'
FILE_HEADS = {
    LIGHT: ['// TG_Shape: the per-vertex and per-face lighting phases of MultiTransformShape -- hot-colour and base colour,',
            '// vertex lights, fog and highlight, face lights and the triangle queue. The orchestrator is in tgl_transform.cpp.'],
}

PHASES = [
    ('xformGpuOnlyShape', 133, 168, 'Claim the hardware vertex slot of a GPU-only shape that has no software vertices.'),
    ('xformVertexProject', 242, 291, 'Project one vertex to screen space and track the on-screen flags and preview bounds.', {'dedent': '\t'}),
    ('xformVertexBaseColor', 293, 431, 'Seed one vertex colour from the hot-colour markers and the base vertex colour.', {'file': LIGHT, 'dedent': '\t'}),
    ('xformVertexLights', 433, 752, 'Accumulate the world lights into one vertex colour.', {'file': LIGHT, 'dedent': '\t'}),
    ('xformVertexFog', 754, 839, 'Write one vertex fog colour and apply the highlight.', {'file': LIGHT, 'dedent': '\t'}),
    ('xformFaceLight', 864, 998, 'Accumulate the world lights into one visible face colour.', {'file': LIGHT, 'dedent': '\t\t'}),
    ('xformFaceCorners', 1000, 1041, 'Add the face colour to each corner vertex colour.', {'file': LIGHT, 'dedent': '\t\t'}),
    ('xformFaceQueue', 1043, 1081, 'Queue the face triangle with its texture and render flags.', {'file': LIGHT, 'dedent': '\t\t'}),
    ('xformEmitShape', 1091, 1124, 'Queue the whole shape for the modern shader path.'),
]
