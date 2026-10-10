# gos_static_prop_batcher split: shared state and initialisation order (2026-10-10)

Decision: pieces are contiguous slices of the old file, in file order, and the build lists them in that order. No state struct:
every shared name keeps its spelling and initialiser, so every use site is byte-identical and `split_verify` can vouch for "move only".

## Slices (file order; sizes after the cut)
core 375 (gates, counters, SPFLUSH split, eager caches, pool/billboard knobs), state 1,128 (the former anonymous namespace: GPU
handles, ring, coalesce and static-draw state, IBL and material helpers), program 865 (`loadProgramsIfNeeded`, ring capacity,
late-register telemetry), mapload 487 (map load/unload, `registerType`), finalize 2,054 (`finalizeGeometry`, one function),
submit 1,259 (`submit`, `submitMultiShape`, bucket upload, V6 arrays, depth prepass), flush 2,366 (`flush`, one function),
shadow 988 (prop shadows, VK shadow-island feed), seam 1,125 (persistent static, `batcher_*` seam accessors, base-instance table).
`finalizeGeometry` and `flush` are single functions of 2,050 and 2,362 lines; their pieces hold nothing else and stay above 1,500.

## What is shared
- The two anonymous namespaces that held state are now one named namespace, `gos_static_prop_batcher_detail`, reached through a
  using-directive (lookup is unchanged); the cut between "state" and "program" reopens it. Shared names lose `static` and are
  declared once in `gos_static_prop_batcher_internal.h` (`extern` variables, prototypes), inside that namespace where they live there.
- Types, macros, constants, mid-file includes and pure declarations that more than one slice needs, and the inline helpers that
  several slices call, sit in the header (`popShortName`, `popIndex`, `initTraceOnce`, ... bodies move after the externs they use).

## Initialisation order (the STOP rule)
- Eager namespace-scope gate caches stay eager and keep their slice, in file order: `static const bool s_x = gate::get<...>()` becomes
  `extern const bool s_x = ...` only where another slice reads it. Dynamic initialisers inside one slice run in their original order.
- Initialisers that read other namespace-scope variables (`sp_deps.py`): `s_nightTagScale` after `s_nightTagMode`,
  `s_farBackdropTintParsed` after `s_farBackdropTint`, `s_iblShSetEnvOverride` after `s_iblShSetEnvOverrideText`. Each pair sits in the
  state slice, adjacent, in its original order; no cross-piece initialiser dependency exists.
- Function-local lazy statics (`iblShEnabled`, `pbrV1Enabled`, `texTier`, `staticPropAmbientV1Enabled`, the first-use reads) are
  untouched: still lazy, still one read. No lazy static became eager and no eager one became lazy.
- Link-order caveat: the pieces are listed consecutively in `GameOS/gameos/CMakeLists.txt` in slice order; across translation units the
  language leaves the order unspecified, and the only cross-piece effect of a different order would be the order in which owner
  constructors (`RenderCore::GpuBufferOwner`) append to the buffer ledger.

## Pins
`check-shader-variant` reads the program build from the program piece, the raw-GL colormask and depthfunc allowlists name the submit and
flush pieces, `check-mdi-submission-ownership` requires `mdi_submit.h` in the flush piece (added there).
