# Phase split spec for GpuMechBatcher::flush (GameOS/gameos/gos_mech_batcher_flush.cpp).
# Line numbers are 1-based lines of the committed file at BASE the split starts from.
# Commit whose text the split started from (the phase split itself is the next commit).
BASE = '49f0a5187'
SRC = 'GameOS/gameos/gos_mech_batcher_flush.cpp'
FUNC_SIG = r'^void GpuMechBatcher::flush\(\)'
STRUCT = 'MechFlushFrame'
FRAME_VAR = 'f'
# Phases that use the shared locals are members of the frame struct, so their bodies name those locals directly.
FRAME_METHODS = True

# Function-local types that more than one phase needs: moved out to file scope unchanged.
HOIST = [237, 362]           # BucketKey, DrawCall

# Locals that live across phases. Declaration lines are lifted into the struct; the statement that
# declared each one stays in its phase as an assignment (or disappears when it was a bare declaration).
MEMBERS = [
    ('totalBones', 'size_t'),
    ('buckets', 'std::map<BucketKey, std::vector<uint32_t>>'),
    ('totalInstances', 'size_t'),
    ('instDst', 'GpuMechInstance*'),
    ('boneDst', 'GpuMechBone*'),
    ('actorBoneBase', 'std::vector<uint32_t>'),
    ('drawCalls', 'std::vector<DrawCall>'),
    ('guardSsbo2', 'std::optional<mc2gl::GlScopedSsboBinding>'),
    ('prevDepthFunc', 'GLint'),
    ('prevDepthTest', 'GLboolean'),
    ('prevDepthMask', 'GLboolean'),
    ('prevBlend', 'GLboolean'),
    ('prevCull', 'GLboolean'),
    ('prevCullMode', 'GLint'),
    ('prevProgram', 'GLint'),
    ('prevVao', 'GLint'),
    ('prevArrayBuf', 'GLint'),
    ('prevElemBuf', 'GLint'),
    ('prevActiveTex', 'GLint'),
    ('prevSampler', 'GLint'),
    ('guardSsbo0', 'std::optional<mc2gl::GlScopedSsboBinding>'),
    ('guardSsbo1', 'std::optional<mc2gl::GlScopedSsboBinding>'),
    ('prevTexUnit0', 'GLint'),
    ('prevTexUnit1', 'GLint'),
    ('prevSampler1', 'GLint'),
    ('prevTexUnit2', 'GLint'),
    ('prevSampler2', 'GLint'),
    ('prevTexUnit3', 'GLint'),
    ('prevSampler3', 'GLint'),
    ('prevTexUnit4', 'GLint'),
    ('prevSampler4', 'GLint'),
    ('prevTexUnit6', 'GLint'),
    ('prevSampler6', 'GLint'),
    ('prevTexUnit7', 'GLint'),
    ('prevSampler7', 'GLint'),
    ('drawnCalls', 'uint32_t'),
]

# Statements a member-declaration rule cannot rewrite on its own: {line: (old text, new text)}.
EDITS = {
    287: ('std::vector<uint32_t> actorBoneBase(s_pendingSubmits.size());',
          'actorBoneBase.assign(s_pendingSubmits.size(), 0u);'),
    269: ('ensureRingCapacity(totalInstances, totalBones);',
          'ensureRingCapacity(f.totalInstances, f.totalBones);'),
}

# Declared in the orchestrator, just before the phase that starts at the key line.
ORCH_BEFORE = {229: 'MechFlushFrame f;'}

FRAME_DOC = ('// Locals the flush phases hand to each other: the bucketed submits, the ring slice pointers,\n'
             '// the draw-call list and the GL state saved before the draw loop and restored after it.')

ORCH_DOC = ('// Per-frame mech draw: bucket the pending submits, stream the instance + bone SSBOs, draw one instanced\n'
            '// call per bucket, restore GL state, then persist the frame for the shadow pass, the VK island and\n'
            '// the extract snapshot. Each phase is a helper above, in execution order.')

PHASES = [
    ('traceLatch', 155, 203,
     'Latch the submit count and run the periodic and boot-time trace setup.'),
    ('resolveSurfaceMaterial', 215, 227,
     'Resolve the named surface-material profile indices once, after the material tables exist.'),
    ('buildBuckets', 229, 267,
     'Count bones and group the pending submits into draw buckets keyed by packet, texture and flags.'),
    ('advanceRing', 275, 284,
     'Advance the ring slot, wait for its oldest fence and point at the slot slices.'),
    ('writeBones', 286, 293,
     'Write each actor bone palette into the bone ring and record its base offset.'),
    ('buildMaterialTable', 295, 359,
     'Assign each bucket a material-table row and upload the table when it grew.'),
    ('writeInstances', 361, 443,
     'Write the instance ring in bucket order and collect the draw calls.'),
    ('validateMaterialTable', 445, 473,
     'Check every draw call material row still names the bucket texture.'),
    ('bindSsbos', 475, 506,
     'Bind the instance, bone and material SSBOs for the draw loop.'),
    ('saveGlState', 508, 561,
     'Save the GL state the draw loop changes so it can be restored afterwards.'),
    ('applyMechPipeline', 563, 631,
     'Apply the mech pipeline and bind the shared VAO, IBO, sampler and PBR detail textures.'),
    ('uploadShadingUniforms', 633, 731,
     'Upload the per-flush shading, PBR and lighting uniforms.'),
    ('uploadProjectionUniforms', 733, 815,
     'Upload the light-pool and projection uniforms and run the view-uniform probe.'),
    ('drawBuckets', 817, 972,
     'Order the SSBO writes before the draws, then issue one instanced draw per bucket.'),
    ('restoreGlState', 974, 1016,
     'Restore the GL state saved before the draw loop.'),
    ('fenceAndInvalidate', 1018, 1026,
     'Fence the ring slot and invalidate the engine render-state cache.'),
    ('emitBatcherStats', 1028, 1054,
     'Print the per-frame batcher and fallback summary (a free function: its block-scope extern must name the global).',
     {'free': True}),
    ('persistDrawCalls', 1056, 1068,
     'Keep this frame draw-call list and SSBO sizes for the shadow pass.'),
    ('snapshotForVkIsland', 1071, 1086,
     'Copy the instance and bone palettes and the view matrix out for the VK mech island.',
     'MC2_VULKAN_ISLAND'),
    ('persistExtraction', 1089, 1118,
     'Persist per-actor facts for the extract snapshot.'),
]
