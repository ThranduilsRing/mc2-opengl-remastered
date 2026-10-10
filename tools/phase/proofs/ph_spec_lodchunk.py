# Phase split spec for gos_TerrainLodChunk_SubmitDrawCommands (GameOS/gameos/gos_terrain_lod_chunk_draw.cpp).
# Line numbers are 1-based lines of the file at BASE.
BASE = 'cb66d632c'
SRC = 'GameOS/gameos/gos_terrain_lod_chunk_draw.cpp'
FUNC_SIG = r'^void gos_TerrainLodChunk_SubmitDrawCommands\('
STRUCTS = {'f': 'TerrainLodSubmitFrame'}
FRAME_METHODS = True
FRAME_GLOBAL = True

# The original parameters, plus the orchestrator-owned locals (the MVP pointer and the static submit counter) the
# phases name; each helper takes the ones its body uses.
PARAMS = [
    ('count', 'int'),
    ('cmds', 'const TerrainDrawCommand*'),
    ('skirtDepths', 'const float*'),
    ('skirtEdgeMasks', 'const unsigned char*'),
    ('edgeStitch', 'const unsigned int*'),
    ('shadowTiers', 'const int*'),
    ('morphFactors', 'const float*'),
    ('mvp', 'const float*'),
    ('s_submitCount', 'int&'),
]

# Locals that live across phases (GL state saved for the restore, the RAII state guards, the feature flags).
MEMBERS = [
    ('prevProg', 'GLint', 'f', [97]),
    ('prevVAO', 'GLint', 'f', [99]),
    ('useGuards', 'bool', 'f', [0]),
    ('gDepthTest', 'std::optional<mc2gl::GlScopedCapability>', 'f', [0]),
    ('gBlend', 'std::optional<mc2gl::GlScopedCapability>', 'f', [0]),
    ('gCull', 'std::optional<mc2gl::GlScopedCapability>', 'f', [0]),
    ('gDepthState', 'std::optional<mc2gl::GlScopedDepthState>', 'f', [125]),
    ('prevCullFace', 'GLboolean', 'f', [126]),
    ('prevDepthTest', 'GLboolean', 'f', [127]),
    ('prevDepthMask', 'GLboolean', 'f', [128]),
    ('prevBlend', 'GLboolean', 'f', [129]),
    ('prevDepthFunc', 'GLint', 'f', [130]),
    ('visualDisplaceActive', 'bool', 'f', [0]),
    ('visualDampActive', 'bool', 'f', [0]),
    ('geomorphActive', 'bool', 'f', [0]),
    ('s_vkTerrainEnabled', 'bool', 'f', [0], 'MC2_VULKAN_ISLAND'),
    ('s_vkTerrainActive', 'bool', 'f', [0], 'MC2_VULKAN_ISLAND'),
    ('s_vkTerrainGlAudit', 'TerrainGlFallbackAudit', 'f', [996], 'MC2_VULKAN_ISLAND'),
    ('s_vkTerrainGlAuditOn', 'bool', 'f', [0], 'MC2_VULKAN_ISLAND'),
]

EDITS = {
    122: ('const bool useGuards =', 'useGuards ='),
    124: ('std::optional<mc2gl::GlScopedCapability> gDepthTest, gBlend, gCull;', ''),
    187: ('const bool visualDisplaceActive =', 'visualDisplaceActive ='),
    197: ('const bool visualDampActive =', 'visualDampActive ='),
    221: ('const bool geomorphActive =', 'geomorphActive ='),
    972: ('const bool s_vkTerrainEnabled =', 's_vkTerrainEnabled ='),
    983: ('const bool s_vkTerrainActive =', 's_vkTerrainActive ='),
    997: ('const bool s_vkTerrainGlAuditOn =', 's_vkTerrainGlAuditOn ='),
}

ORCH_BEFORE = {97: '    TerrainLodSubmitFrame f;'}

FRAME_DOC = ('// Locals the terrain LOD-chunk submit phases share: the GL state saved for the restore, the RAII state guards\n'
             '// (destroyed after the restore, in reverse declaration order, as they were as locals) and the feature flags.')

ORCH_DOC = ('// Submits the LOD chunk draw commands. The early bail-outs, the top-level pass guard and the MVP fetch stay here;\n'
            '// each phase below is a verbatim helper, run in the original order.')

PHASES = [
    ('saveGlStateAndGuards', 97, 147, 'Save the GL state the draw changes and apply the terrain depth/blend/cull state.'),
    ('bindProgramAndVisualHeight', 149, 269, 'Apply the terrain pipeline, bind the program, VAO and height SSBOs and set the displacement uniforms.'),
    ('bindAtlasAndShadowMaps', 271, 443, 'Bind the colormap atlas (with its UV params), the shadow maps and the light matrices.'),
    ('bindAuxTextures', 445, 513, 'Bind the material normal array, the cement atlas, the transition mask and the control map.'),
    ('bindMaterialAndOverlay', 515, 569, 'Bind the per-layer material albedo array and the overlay-v2 sidecar.'),
    ('bindShorelineMask', 571, 659, 'Bind the shoreline mask and set the wet/foam band uniforms.'),
    ('uploadTerrainTuning', 661, 937, 'Upload the material tiling, tint, lighting and shadow tuning uniforms.'),
    ('noteFirstSubmit', 939, 947, 'Log the first non-empty submit and reset the zero-submit streak.'),
    ('countSubmitAndPrepVk', 950, 998, 'Count the submit, reset the displaced-chunk counters and prepare the VK terrain island.'),
    ('drawChunks', 1000, 1256, 'Issue the draw for every queued command, then report the per-band displaced-chunk counts.'),
    ('auditAndMarkDrawn', 1258, 1322, 'Report the patches the VK island left to GL and mark terrain drawn for the post-process passes.'),
    ('restoreGlState', 1323, 1343, 'Unbind the terrain buffers and restore the saved GL state.'),
    ('submitVkIsland', 1344, 1395, 'Hand the captured LOD0 patches to the VK island once GL state is restored.'),
]
