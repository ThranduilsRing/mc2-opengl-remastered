# Phase split spec for TerrainQuad::draw (mclib/quad_draw.cpp). Line numbers are 1-based lines of the file at BASE.
BASE = 'ee24d9d1d'
SRC = 'mclib/quad_draw.cpp'
FUNC_SIG = r'^void TerrainQuad::draw \(void\)'
METHOD_CLASS = 'TerrainQuad'
INDENT = '\t'
DEDENT = '	'
STRUCTS = {'f': 'QuadDrawFrame', 'fp': 'QuadFastFrame'}

# Locals that live across phases. The statement that declared each one stays in its phase as an
# assignment (or disappears when it was a bare declaration).
MEMBERS = [
    ('gVertex[3]', 'gos_VERTEX', 'f'),
    ('minU', 'float', 'f'),
    ('maxU', 'float', 'f'),
    ('minV', 'float', 'f'),
    ('maxV', 'float', 'f'),
    ('oldminU', 'float', 'f'),
    ('oldmaxU', 'float', 'f'),
    ('oldminV', 'float', 'f'),
    ('oldmaxV', 'float', 'f'),
    ('fastPathEligible', 'bool', 'f'),
    ('pzTri1', 'bool', 'fp'),
    ('pzTri2', 'bool', 'fp'),
]

EDITS = {
    171: ('const bool fastPathEligible =', 'fastPathEligible ='),
    225: ('if (fastPathEligible)', 'if (f.fastPathEligible)'),
    263: ('bool pzTri1, pzTri2;', ''),
    334: ('if (!pzTri1 && !pzTri2) return;', 'if (!fp.pzTri1 && !fp.pzTri2) return;'),
}

ORCH_BEFORE = {
    99: '\t\tQuadDrawFrame f;',
    227: '\t\t    QuadFastFrame fp;',
}

FRAME_DOC = ('// Locals TerrainQuad::draw hands from one phase to the next: the shared vertex scratch and UV\n'
             '// insets (QuadDrawFrame) and the per-triangle pz-validity flags of the fast path (QuadFastFrame).')

ORCH_DOC = ('// Emits this quad: the M2 fast path (thin record, overlay decals) when the patch stream is armed,\n'
            '// otherwise the legacy per-triangle gos_VERTEX path for the BOTTOMRIGHT / BOTTOMLEFT split.')

PHASES = [
    ('drawSetup', 99, 151,
     'Set up the vertex scratch and the base / overlay UV insets for this quad.'),
    ('drawPathSelect', 153, 223,
     'Decide whether the M2 fast path takes this quad and run the path-mix diagnostic.'),
    ('fastPzGate', 227, 332,
     'Fast path: compute the pz-validity of each triangle and run the pz parity probe.'),
    ('fastThinEmit', 336, 471,
     'Fast path: emit the thin record (falls through to the detail label when the recipe SSBO is full).'),
    ('fastOverlayEmit', 472, 539,
     'Fast path: emit the overlay decal triangles.'),
    ('drawLegacyBottomRight', 547, 958,
     'Legacy path for a BOTTOMRIGHT quad: base, overlay, detail and patch-stream emit for both triangles.'),
    ('drawLegacyBottomLeft', 962, 1326,
     'Legacy path for a BOTTOMLEFT quad: base, overlay, detail and patch-stream emit for both triangles.'),
]

# The legacy bodies declare their own pzTri1/pzTri2; the fast-path flags must not alias into them.
ONLY = {'fp': ['fastPzGate', 'fastThinEmit', 'fastOverlayEmit']}
