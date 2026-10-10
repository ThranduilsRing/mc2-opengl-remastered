# Phase split spec for GpuStaticPropBatcher::flush (GameOS/gameos/gos_static_prop_batcher_flush.cpp).
# Line numbers are 1-based lines of the file at BASE.
BASE = '115c3ea60'
SRC = 'GameOS/gameos/gos_static_prop_batcher_flush.cpp'
FUNC_SIG = r'^void GpuStaticPropBatcher::flush\('
METHOD_CLASS = 'GpuStaticPropBatcher'
INDENT = '    '
DEDENT = ''
STRUCTS = {'f': 'StaticPropFlushFrame'}
ALIAS_PER_LINE = 4

# The orchestrator keeps the RAII guard, the Tracy zone / hitch scope, the timing statics and clocks, the early
# bail-outs, the coalesce / legacy branch skeleton and the dispatch chain; helpers get what they name.
PARAMS = [
    ('snap', 'const RenderSnapshot*'),
    ('v6Packets', 'std::vector<RenderCore::DrawPacket>&'),
    ('v6Meta', 'std::vector<StaticPropDispatchMeta>&'),
]

MEMBERS = [
    ('parityBuffer', 'GLuint', 'f', [105]),
    ('guardProgram', 'std::optional<mc2gl::GlScopedProgram>', 'f', [162]),
    ('guardVao', 'std::optional<mc2gl::GlScopedVertexArray>', 'f', [163]),
    ('guardArrayBuf', 'std::optional<mc2gl::GlScopedBufferBinding>', 'f', [164]),
    ('guardElemBuf', 'std::optional<mc2gl::GlScopedBufferBinding>', 'f', [165]),
    ('guardTexUnit0', 'std::optional<mc2gl::GlScopedTextureUnit>', 'f', [166]),
    ('guardSsbo0', 'std::optional<mc2gl::GlScopedSsboBinding>', 'f', [167]),
    ('guardSsbo1', 'std::optional<mc2gl::GlScopedSsboBinding>', 'f', [168]),
    ('guardSsbo2', 'std::optional<mc2gl::GlScopedSsboBinding>', 'f', [169]),
    ('guardSsbo3', 'std::optional<mc2gl::GlScopedSsboBinding>', 'f', [170]),
    ('guardSsbo5', 'std::optional<mc2gl::GlScopedSsboBinding>', 'f', [171]),
    ('locParityWrite', 'GLint', 'f', [0]),
    ('locParityVerts', 'GLint', 'f', [0]),
    ('locParityBaseVertex', 'GLint', 'f', [0]),
    ('useC1bIndirect', 'bool', 'f', [0]),
    ('fr_off_bytes_d', 'size_t', 'f', [0]),
    ('off_total_bytes', 'size_t', 'f', [0]),
    ('on_total_bytes', 'size_t', 'f', [0]),
    ('guardSsbo4', 'std::optional<mc2gl::GlScopedSsboBinding>', 'f', [575]),
    ('guardTex2DArray', 'std::optional<mc2gl::GlScopedTextureUnit>', 'f', [576]),
    ('runV6', 'bool', 'f', [0]),
    ('v6LockstepViolations', 'uint32_t', 'f', [1025]),
    ('retireLiveBuilder', 'bool', 'f', [1028]),
    ('runV5', 'bool', 'f', [0]),
]

EDITS = {
    241: ('const GLint locParityWrite      =', 'locParityWrite ='),
    242: ('const GLint locParityVerts      =', 'locParityVerts ='),
    243: ('const GLint locParityBaseVertex =', 'locParityBaseVertex ='),
    288: ('const bool useC1bIndirect =', 'useC1bIndirect ='),
    550: ('const size_t fr_off_bytes_d =', 'fr_off_bytes_d ='),
    554: ('const size_t off_total_bytes =', 'off_total_bytes ='),
    557: ('const size_t on_total_bytes  =', 'on_total_bytes  ='),
    1021: ('const bool runV6 =', 'runV6 ='),
    1098: ('const bool runV5 =', 'runV5 ='),
    1100: ('if (runV6 && v6LockstepViolations == 0u) {', 'if (f.runV6 && f.v6LockstepViolations == 0u) {'),
    1504: ('} else if (runV5) {', '} else if (f.runV5) {'),
}

ORCH_BEFORE = {88: '    StaticPropFlushFrame f;'}

FRAME_DOC = '// Locals the static-prop flush phases share.'

ORCH_DOC = ('// Draws the frame static props. The top-level pass guard, the timing clocks, the early bail-outs and the coalesce /\n'
            '// legacy branch skeleton stay here; each phase body is a member helper declared in gos_static_prop_batcher.h.')

PREP = 'GameOS/gameos/gos_static_prop_batcher_flush_prep.cpp'
DISP = 'GameOS/gameos/gos_static_prop_batcher_flush_dispatch.cpp'
# Every helper that issues an indirect draw stays in the orchestrator file, next to mdi_submit::trace() (the
# TU-level rule of scripts/check-mdi-submission-ownership.py).
FILE_HEADS = {
    PREP: ['// GpuStaticPropBatcher: the flush setup phases -- candidate log, GL guards, coalesce instance write, alpha flags',
           '// and the coalesce program uniforms. The orchestrator is in gos_static_prop_batcher_flush.cpp.'],
    DISP: ['// GpuStaticPropBatcher: the flush gate arming, DrawPacket build, v5 / v6 dispatch, legacy setup and the fence /',
           '// restore tail. The orchestrator is in gos_static_prop_batcher_flush.cpp.'],
}

PHASES = [
    ('candidateLogAndParityReadback', 88, 144, 'Emit the candidate log and read back / reset the parity output buffer.', {'file': PREP}),
    ('acquireGlGuards', 146, 181, 'Save the program, VAO, buffer, texture-unit and SSBO bindings for the restore at exit.', {'file': PREP}),
    ('applyOpaqueState', 182, 264, 'Apply the opaque pipeline state, bind the shared VAO / IBO and per-type SSBO, clear the parity slot.'),
    ('prepareIndirectAndFence', 266, 334, 'Decide the compute-cull indirect draw and wait for the coalesce fence.', {'file': PREP}),
    ('writeCoalesceInstances', 337, 446, 'Write each type bucket into the coalesce instance map.', {'file': PREP}),
    ('logInstDiagPath', 456, 476, 'One-shot path diagnostic for the instance draw.', {'file': PREP}),
    ('refreshAlphaFlags', 481, 549, 'Re-resolve the per-packet alpha-test flags into the per-draw SSBO.', {'file': PREP}),
    ('bindCoalesceGuards', 550, 578, 'Compute the group byte ranges and save SSBO slot 4 and the 2D-array texture binding.', {'file': PREP}),
    ('uploadCoalesceMvp', 585, 615, 'Upload the object MVP to the coalesce program.', {'file': PREP}),
    ('uploadCoalesceUniforms', 616, 789, 'Upload the shared uniforms to the coalesce program.', {'file': PREP}),
    ('bindPerDrawAndMaterial', 790, 871, 'Bind the per-draw SSBO and the material tables.', {'file': PREP}),
    ('armV5', 872, 909, 'Run the one-time DrawPacket v5 gate-arm checks.', {'file': DISP}),
    ('armV6', 911, 961, 'Run the one-time DrawPacket v6 gate-arm checks.', {'file': DISP}),
    ('drawStaticPopulation', 963, 1013, 'Draw the static-population split when its gate is armed.'),
    ('decideV6', 1020, 1021, 'Decide whether the DrawPacket v6 path runs this frame.', {'file': DISP}),
    ('initV6State', 1025, 1028, 'Reset the v6 lock-step and live-builder state.', {'file': DISP}),
    ('buildV6Packets', 1030, 1093, 'Build and validate the DrawPacket v6 arrays.', {'file': DISP}),
    ('decideV5', 1095, 1098, 'Decide whether the DrawPacket v5 path runs this frame.', {'file': DISP}),
    ('dispatchV6', 1101, 1503, 'DrawPacket v6 dispatch.', {'file': DISP}),
    ('dispatchV5', 1505, 1666, 'DrawPacket v5 per-draw-call dispatch.', {'file': DISP}),
    ('dispatchBc7Buckets', 1669, 1718, 'BC7 bucketed multidraw dispatch.'),
    ('dispatchMultiDraw', 1720, 1773, 'Existing indirect multidraw dispatch.'),
    ('finishCoalesceDraw', 1776, 1806, 'Unbind the indirect buffer and ORM unit, restore the saved bindings and latch the first flush.', {'file': DISP}),
    ('legacyPrepare', 1809, 1979, 'Legacy path: sync the instance / color SSBOs and set up the program.', {'file': DISP}),
    ('legacyCensus', 1981, 1999, 'One-shot per-type draw census.', {'file': DISP}),
    ('legacyDrawLoop', 2000, 2211, 'Legacy path: draw each type.'),
    ('fenceAndParitySlot', 2214, 2231, 'Fence the ring slots and hand the parity slot to the dual-emit state.', {'file': DISP}),
    ('releaseGlGuards', 2233, 2248, 'Restore the GL state saved at flush start.', {'file': DISP}),
    ('finishFlush', 2249, 2291, 'Accumulate the counters, tick parity, clear the frame buckets and invalidate the render-state cache.', {'file': DISP}),
]

FRAME_HEADER = 'GameOS/gameos/gos_static_prop_flush_frame.h'
FRAME_HEADER_DOC = 'Locals the GpuStaticPropBatcher::flush phases hand to each other.'
INTERNAL_INCLUDES = ['#include "gos_static_prop_batcher_internal.h"',
                     '#include "mdi_submit.h"                          // MDI-SUBMISSION-SCAFFOLD-1: flush issues the indirect draws']
PREFIX_EDITS = {1: '// GpuStaticPropBatcher: GpuStaticPropBatcher::flush -- the orchestrator and the setup / coalesce-write helpers.'}
