# Phase split spec for MLRLightMap::DrawLightMaps (mclib/mlr/mlrlightmap.cpp). Line numbers are 1-based lines of the file at
# BASE.
BASE = 'd9b32d650'
SRC = 'mclib/mlr/mlrlightmap.cpp'
FUNC_SIG = r'^void MLRLightMap::DrawLightMaps\('
METHOD_CLASS = 'MLRLightMap'
INDENT = '\t'
DEDENT = ''
STRUCTS = {}
FRAME_GLOBAL = True
MEMBERS = []
EDITS = {}
ORCH_BEFORE = {}
ORCH_EXTRA = []

# The orchestrator keeps the stream loop, the record switch, the polygon set-up and vertex transform with their
# clipping-state locals, and the final flush; each helper gets the loop locals it names, by reference.
PARAMS = [
    ('sorter', 'MLRSorter*'),
    ('i', 'int&'),
    ('stride', 'unsigned short&'),
    ('argb', 'DWORD&'),
    ('coords', 'Stuff::Point3D*&'),
    ('colors', 'Stuff::RGBAColor*&'),
    ('texCoords', 'Vector2DScalar*&'),
    ('gos_vertices', 'GOSVertex*&'),
    ('numGOSVertices', 'int&'),
    ('currentState', 'MLRState&'),
    ('masterState', 'MLRState&'),
    ('theAnd', 'MLRClippingState&'),
    ('theOr', 'MLRClippingState&'),
    ('cs', 'MLRClippingState*&'),
    ('v4d', 'Stuff::Vector4D*&'),
    ('k', 'int&'),
    ('k0', 'int&'),
    ('k1', 'int&'),
    ('l', 'int&'),
    ('mask', 'int&'),
    ('ct', 'int&'),
    ('a', 'Stuff::Scalar&'),
    ('numberVerticesPerPolygon', 'int&'),
    ('firstIsIn', 'bool&'),
    ('theTest', 'MLRClippingState&'),
]

ORCH_DOC = ('//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~\n'
            '// Replays the recorded light-map stream into GOS triangles. The stream loop, the record switch, the polygon\n'
            '// set-up and vertex transform and the final flush stay here; the state flush and the polygon fast, one-plane,\n'
            '// multi-plane and triangulation paths are helpers declared in mlrlightmap.hpp, run in the original order.')

S = {'dedent': '\t\t\t\t'}
PHASES = [
    ('flushForLightMapState', 235, 267, 'Flush the pending triangles when the light-map render state changes.', {'dedent': '\t\t\t'}),
    ('polygonFastPath', 311, 327, 'Triangulate a polygon that needs no clipping.', {'dedent': '\t\t\t\t'}),
    ('polygonClipOnePlane', 353, 522, 'Clip a polygon against a single plane into the clip buffer.', {'dedent': '\t\t\t\t\t\t'}),
    ('polygonClipManyPlanes', 533, 789, 'Clip a polygon against several planes, plane by plane.', {'dedent': '\t\t\t\t\t\t'}),
    ('polygonTriangulateClipped', 794, 810, 'Triangulate the clipped polygon.', {'dedent': '\t\t\t\t\t'}),
    ('colorPolygonFastPath', 857, 870, 'Triangulate a per-vertex-colour polygon that needs no clipping.', {'dedent': '\t\t\t\t'}),
    ('colorPolygonClipOnePlane', 896, 1080, 'Clip a per-vertex-colour polygon against a single plane.', {'dedent': '\t\t\t\t\t\t'}),
    ('colorPolygonClipManyPlanes', 1091, 1368, 'Clip a per-vertex-colour polygon against several planes.', {'dedent': '\t\t\t\t\t\t'}),
    ('colorPolygonTriangulateClipped', 1373, 1386, 'Triangulate the clipped per-vertex-colour polygon.', {'dedent': '\t\t\t\t\t'}),
]
