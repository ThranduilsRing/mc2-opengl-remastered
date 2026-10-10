# Phase split spec for Terrain::init (mclib/terrain_init.cpp). Line numbers are 1-based lines of the file at BASE.
BASE = '8eddeaeaf'
SRC = 'mclib/terrain_init.cpp'
FUNC_SIG = r'^long Terrain::init\('
METHOD_CLASS = 'Terrain'
INDENT = '\t'
DEDENT = ''
STRUCTS = {'lf': 'TerrainLodInitFrame'}

# Parameters of Terrain::init that the phases read (each helper takes the ones its body names).
PARAMS = [
    ('verticesPerMapSide', 'unsigned long'),
    ('pakFile', 'PacketFile*'),
    ('visibleVertices', 'unsigned long'),
    ('percent', 'volatile float&'),
    ('percentRange', 'float'),
]

# Locals that live across the LOD-chunk sidecar phases (the heightfield and per-vertex material copies).
MEMBERS = [
    ('elev', 'std::vector<float>', 'lf'),
    ('ttype', 'std::vector<float>', 'lf'),
]

EDITS = {
    223: ('std::vector<float> elev((size_t)n);', 'elev.assign((size_t)n, 0.0f);'),
    224: ('std::vector<float> ttype((size_t)n);', 'ttype.assign((size_t)n, 0.0f);'),
}

ORCH_BEFORE = {222: '\t\t\tTerrainLodInitFrame lf;'}

FRAME_DOC = ('// Per-vertex heightfield and material copies the terrain LOD-chunk load phases of Terrain::init share.')

ORCH_DOC = ('// Mission terrain load: heap, textures, block lists, map data, the optional LOD-chunk metadata and\n'
            '// sidecar uploads, then the vertex / quad lists and the colour-map and material-profile setup.\n'
            '// Each phase is a member helper declared in terrain.h, in execution order.')

LOD = {'dedent': '\t\t'}
PHASES = [
    ('initMapExtents', 12, 26, 'Derive the map extents and block counts from the vertex count.'),
    ('initTerrainHeap', 28, 38, 'Create the terrain heap on first load.'),
    ('initTerrainTextures', 39, 58, 'Create the terrain texture set on first load.'),
    ('initColorMapTexture', 64, 93, 'Pick the colour-map name and create the colour map when its image exists.'),
    ('initObjBlockLists', 96, 124, 'Allocate the object-block, active-vertex and mover / used block lists.'),
    ('initMapData', 126, 160, 'Create the map data manager and upload the height texture.'),
    ('lodAllocMeta', 166, 218, 'LOD chunk: allocate and initialise the block and superchunk metadata.',
     {'dedent': '\t'}),
    ('lodHeightUpload', 222, 238, 'LOD chunk: upload the full heightfield and per-vertex material to the GPU.', LOD),
    ('lodControlMap', 240, 331, 'LOD chunk: load the control-map sidecar when its gate is on.', LOD),
    ('lodOverlayV2', 333, 452, 'LOD chunk: load the overlay-v2 sidecar when its gate is on.', LOD),
    ('lodShorelineMask', 454, 593, 'LOD chunk: load the shoreline-mask sidecar when its gate is on.', LOD),
    ('lodWaterShore', 595, 682, 'LOD chunk: build the water-shore edge data when the shore mode asks for it.', LOD),
    ('lodVisualHeight', 684, 1005, 'LOD chunk: load the 4x visual heightfield bake when its gate is on.', LOD),
    ('lodFlagConcrete', 1007, 1024, 'LOD chunk: flag the blocks that contain concrete so the LOD pass keeps them fine.', LOD),
    ('initVertexQuadLists', 1028, 1043, 'Allocate the vertex and quad lists and the map cell arrays.'),
    ('initColorMapLoad', 1045, 1053, 'Start loading the colour map when it exists.'),
    ('initMaterialProfile', 1055, 1076, 'Select the terrain material profile for this mission.'),
]
