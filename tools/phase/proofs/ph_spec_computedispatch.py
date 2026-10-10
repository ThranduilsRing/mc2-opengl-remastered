# Phase split spec for gos_terrain_indirect::ComputeDispatch (GameOS/gameos/gos_terrain_indirect_dispatch.cpp).
# Line numbers are 1-based lines of the file at BASE.
BASE = 'f8276081b'
SRC = 'GameOS/gameos/gos_terrain_indirect_dispatch.cpp'
FUNC_SIG = r'^void ComputeDispatch\(\) \{'
INDENT = '    '
DEDENT = ''
STRUCTS = {}
FRAME_GLOBAL = True
MEMBERS = []
EDITS = {}
ORCH_BEFORE = {}
ORCH_EXTRA = []
FILE_NAMESPACE = 'gos_terrain_indirect'
INTERNAL_INCLUDES = ['#include "gos_terrain_indirect_internal.h"']

# The orchestrator keeps the arming guards, the RAII zones, the lighting-SSBO guard, the per-frame ring locals (frame
# index, slot, fence, fingerprint, wait result), the window build, the MVP upload, the dispatch and its barrier and the
# final unbinds; each phase body gets the locals it names.
PARAMS = [
    ('lightSsbo', 'GLuint'),
    ('s_bucketHeaderTrace', 'bool'),
    ('windowCount', 'uint32_t'),
    ('s_solidUseWindow', 'int'),
    ('s_probeSink', 'FILE*'),
    ('s_ringTrace', 'bool'),
    ('ringFrameIdx', 'uint64_t'),
    ('probedSlot', 'int'),
    ('fencePresent', 'bool'),
    ('mvpFp', 'uint32_t&'),
    ('waitResult', 'GLenum&'),
    ('waitUs', 'uint64_t&'),
    ('thinSlotOffset', 'GLintptr'),
    ('mvp', 'const float*'),
    ('fp', 'uint32_t'),
]

ORCH_DOC = ('// Runs the solid-terrain cull/pack compute. The arming guards, the lighting guard, the per-frame ring locals, the\n'
            '// window build, the MVP upload, the dispatch with its barrier and the unbinds stay here; each other phase body is a\n'
            '// helper declared in gos_terrain_indirect_internal.h, run in the original order. A phase that abandons the\n'
            '// dispatch returns true and the orchestrator returns.')

COMPUTE = 'GameOS/gameos/gos_terrain_indirect_compute.cpp'
FILE_HEADS = {
    COMPUTE: ['// gos_terrain_indirect: the ComputeDispatch phases -- program build, ring fence wait and tripwires, counter clears,',
              '// compute bindings and uniforms, the depth-transition probe and the post-dispatch probes. The orchestrator is in',
              '// gos_terrain_indirect_dispatch.cpp.'],
}
C = {'file': COMPUTE}
PHASES = [
    ('computeBuildProgram', 270, 305, 'Build the solid compute program and cache its uniform locations on first use.', {'file': COMPUTE, 'exits': True}),
    ('computeAllocDebugBuffers', 310, 331, 'Allocate the bucket-header and canary SSBOs the first time they are needed.', C),
    ('computeFingerprintMvp', 389, 403, 'Fingerprint the terrain MVP for the ring probe.', C),
    ('computeRingFenceWait', 407, 425, 'Poll the ring slot fence without blocking and release it once signalled.', C),
    ('computeTripwireMiss', 427, 454, 'Report a missing fence past warm-up or a fence timeout.', {'file': COMPUTE, 'uses': ['s_probeSink']}),
    ('computeBucketHeaderReadback', 456, 691, 'Read back the previous bucket header and report the visible-count overshoot probes.', {'file': COMPUTE, 'uses': ['s_probeSink']}),
    ('computeRingSkipGuard', 693, 703, 'Skip the dispatch while the ring slot is still in flight.', {'file': COMPUTE, 'exits': True, 'uses': ['s_probeSink']}),
    ('computeClearCounters', 707, 737, 'Zero the indirect command count and the bucket header and order the clear before the dispatch.', C),
    ('computeBindAndUniforms', 739, 811, 'Bind the compute storage buffers and upload the per-frame uniforms.', {'file': COMPUTE, 'exits': True}),
    ('computeDepthTransitionProbe', 838, 1052, 'Print the zoom-step depth probe on the frame the MVP jumps.', {'file': COMPUTE, 'dedent': '    '}),
    ('computePostDispatchProbes', 1094, 1148, 'Force a GPU drain and read back the command count for the ring probes.', {'file': COMPUTE, 'uses': ['s_probeSink']}),
]
