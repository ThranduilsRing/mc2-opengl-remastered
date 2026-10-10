# gos_terrain_indirect split: shared state design (2026-10-10)

Decision: no `TerrainIndirectState` struct in this split. Every shared name keeps its spelling and its initialiser, so each
use site is byte-identical to the original and `split_verify` can vouch for "move only". Collapsing the ~85 mirrors of GPU
state into one struct is a separate rewrite of every use site; it stays a follow-up.

## What is shared (declared in `gos_terrain_indirect_internal.h`, defined once, in the owning piece)
All of it lives in `namespace gos_terrain_indirect_detail` (the former anonymous namespaces, now one named namespace that
every piece reaches through `using namespace gos_terrain_indirect_detail;`, so lookup is unchanged).
- cement piece: atlas GL handles and geometry (`g_atlas*`, `g_cementAtlas*`), the layer maps (`g_cementLayerIndexBySlot`,
  `g_cementLayerIndexByNodeIdx`, `g_cementAlpha*`), transition-mask handle/ready flag, `g_uniqueTerrainNodeIds`,
  the per-frame cement counters read by the packer, and the classifier entry points the atlas upload calls.
- atlas piece: dense recipe storage (`g_denseRecipes`, dirty flags, `g_recipeSSBO`, `g_recipeMapSide`, `g_recipeReady`),
  the solid-window staging buffer and SSBO, `traceOn`, `s_firstDrawPrintedThisMission`, `BuildColormapAtlas`, `BuildCementCatalogAtlas`.
- pack piece: the thin ring (`g_thinRecordSSBO`, `g_thinRingSlot`, `g_thinRingFences`), indirect command buffer, arming
  flags, compute program and SSBO handles, the dispatch MVP snapshot (`g_dispatchMvp*`, `g_thinSlotMVP*`), the solid uniform
  locations, and `ResourcesReady`, `PackThinRecordsForFrame`, `BuildIndirectCommands`, `UploadTerrainHandleLUT`, `BuildSolidQuadWindowSSBO`.
- header (constants, inline helpers, types): `kThinRingFrames`, `kMaxThinRecords`, `kThinRecordBytes`, `kIndirectCmdBufferBytes`,
  `DrawArraysIndirectCommand`, `CementCellStats`, `RunwayToneScale`, `kConcreteToneGain{Lo,Hi}`, `terrainTypeToMaterialLocal`,
  `InMissionTransition`, the Stage 3 includes and the bridge declaration.

## What stays per-piece
Every other file-scope static keeps its anonymous-namespace or `static` linkage in the piece that uses it: the
mine and decal VBO/texture state and vertex types (one piece), the parity summary counters (diag), the N1 counters and cost-split
roll-ups (core), the cement tunables and heuristics only the cement classifier reads, and the atlas-build locals.

## Initialisation (the STOP rule)
- Eager gate caches stay eager, one definition each, `extern const bool` in the header: `s_loadRaceDelayAtlas` (core),
  `s_thinCanaryEnabled` (core, in `gos_terrain_indirect`), `g_envRingTrace` (pack). Two or more pieces read each of them.
- Function-local `static const bool s = gate::get<...>()` readers (`IsEnabled` and the other `Is*Enabled` / cement gates) are
  untouched: still lazy, still one read.
- Zero-initialised handles and flags keep their `= 0` / `= false` / `= -1` initialisers at the definition.
- No lazy static became eager and no eager one became lazy.

## Pins
`check-buffer-lifetime-ownership` and the raw-GL fbobind allowlist name the dispatch piece (ComputeDispatch's command barrier,
the water reflection pass), `check-object-mvp-currency` allows the pack (snapshot) and dispatch (getter) pieces,
`check-cpu-gpu-cogating` reads the COLORMAP_CPU_RETIRE reader in the cement piece.
