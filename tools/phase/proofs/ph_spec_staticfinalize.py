# Phase split spec for GpuStaticPropBatcher::finalizeGeometry (GameOS/gameos/gos_static_prop_batcher_finalize.cpp).
# Line numbers are 1-based lines of the file at BASE.
BASE = 'ce2c3a688'
SRC = 'GameOS/gameos/gos_static_prop_batcher_finalize.cpp'
FUNC_SIG = r'^void GpuStaticPropBatcher::finalizeGeometry\('
METHOD_CLASS = 'GpuStaticPropBatcher'
INDENT = '    '
DEDENT = ''
STRUCTS = {'f': 'StaticPropFinalizeFrame'}
ALIAS_PER_LINE = 4
PARAMS = []

# Locals that live across the finalize phases.
MEMBERS = [
    ('coalesceStart', 'std::chrono::steady_clock::time_point', 'f', [0]),
    ('typeCount', 'uint32_t', 'f', [0]),
    ('layerForType', 'std::vector<int32_t>', 'f', [0]),
    ('layerForPacket', 'std::vector<int32_t>', 'f', [0]),
    ('uvScaleXByPacket', 'std::vector<float>', 'f', [0]),
    ('uvScaleYByPacket', 'std::vector<float>', 'f', [0]),
]

EDITS = {
    181: ('const auto coalesceStart =', 'coalesceStart ='),
    496: ('std::vector<int32_t> layerForType(typeCount, -1);', 'layerForType.assign(typeCount, -1);'),
    503: ('std::vector<int32_t> layerForPacket(s_packets.size(), -1);', 'layerForPacket.assign(s_packets.size(), -1);'),
    504: ('std::vector<float>   uvScaleXByPacket(s_packets.size(), 1.0f);', 'uvScaleXByPacket.assign(s_packets.size(), 1.0f);'),
    505: ('std::vector<float>   uvScaleYByPacket(s_packets.size(), 1.0f);', 'uvScaleYByPacket.assign(s_packets.size(), 1.0f);'),
    291: ('const uint32_t typeCount =', 'typeCount ='),
}

ORCH_BEFORE = {9: '    StaticPropFinalizeFrame f;'}

FRAME_DOC = '// Locals the static-prop finalize phases share.'

ORCH_DOC = ('// Builds the shared geometry buffers, the coalesce layout, the texture arrays and the per-packet draw tables once per map.\n'
            '// The already-finalized bail-out stays here; each phase is a member helper declared in gos_static_prop_batcher.h.\n'
            '// A helper returns true when the original function would have returned at that point.')

FRAME_HEADER = 'GameOS/gameos/gos_static_prop_finalize_frame.h'
FRAME_HEADER_DOC = 'Locals the GpuStaticPropBatcher::finalizeGeometry phases hand to each other.'
INTERNAL_INCLUDES = ['#include "gos_static_prop_batcher_internal.h"']
TEX = 'GameOS/gameos/gos_static_prop_batcher_finalize_textures.cpp'
FILE_HEADS = {
    TEX: ['// GpuStaticPropBatcher: the texture-array phase of finalizeGeometry -- per-group array build, the BC7 / ORM bucket arrays',
          '// and the per-packet layer tables. The orchestrator is in gos_static_prop_batcher_finalize.cpp.'],
}
PREFIX_EDITS = {1: '// GpuStaticPropBatcher: GpuStaticPropBatcher::finalizeGeometry -- the orchestrator and every phase except the texture arrays.'}

PHASES = [
    ('finalizePrologue', 9, 25, 'Log the texture tier, clear the per-packet side tables and load the programs.'),
    ('createSharedBuffers', 27, 60, 'Create the shared VAO, VBO and IBO and set the vertex attributes.'),
    ('uploadPerTypeAndPublish', 62, 158, 'Upload the per-type SSBO, record the buffer sizes, free the staging data and mark the geometry finalized.'),
    ('layoutCoalesce', 181, 479, 'Decide whether coalesce is wanted and lay out the permutation, the group ranges and the instance pool.', {'exits': True}),
    ('buildTextureArrays', 481, 1448, 'Build the per-group texture arrays and publish the per-packet and per-type layer tables.', {'exits': True, 'file': TEX}),
    ('buildPerPacketDrawTables', 1449, 1910, 'Sort the packets and build the per-draw and command-to-bucket SSBOs.'),
    ('reportMaterialCacheAndBaseInstances', 1912, 1994, 'Report the material cache and allocate the base-instance table and permutation.', {'exits': True}),
    ('armCoalesce', 1996, 2027, 'Arm coalesce and log the finalize summary.'),
]
