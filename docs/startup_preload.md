# Startup preload

A pure-IO background worker started at launch (`startStartupPreload()` in `code/mechcmd2.cpp`, after the mod VFS is mounted) and joined before `FastFileFini()`. It reads files into the OS file cache so the first mission load does not pay cold-disk reads. It is ON; it has no gate.

## Design
- Plan (built on the main thread from main-thread-owned registries): every base FastFile archive first, then every mod-winning `data/` file from the mod VFS (`EnumerateModFiles`, `LookupModAbsolutePath`). Skips `data/movies`, `data/sound*`, `data/music*`.
- Worker: `THREAD_MODE_BACKGROUND`, sequential 1 MiB reads into a private buffer, no engine calls. Mod budget: 96 MB per file, 1.5 GB total.
- Mod-aware by construction: the plan comes from the active mod stack at launch (MC2_ACTIVE_MOD / MC2_MOD_DEPS), so a different stack is a different plan. There is no persistent cache to key; the OS cache is the cache.
- Log lines `[STARTUP_PRELOAD]` print only with `MC2_DIAG_TAGS=STARTUP_PRELOAD`.
- It does not cover per-frame CPU work: the prewarm of player mechs added in `Logistics::beginMission` is covered at the file level (their archive/loose files are in the plan), not at the object level.

## Finding: warm mission load is a CPU parse, not IO
After the files are cached, a stock mission load is still about 15 s, of which about 7.5 s is `Appearance.GetType` / `Shape.Load` parsing on the main thread. `AppearanceTypeList` is mission-owned and erased at mission destroy, so nothing parsed survives into the next load, and nothing can be parsed ahead of the first mission.

## Follow-up design (not built): mission-independent type cache
A registry, owned outside `Mission`, keyed by (appearance name, mod-stack hash), holding the parsed immutable `TG_TypeShape` / `AppearanceType` data. `AppearanceTypeList::getType` would consult it before parsing; mission destroy would drop only mission-local state. A worker could pre-parse the common types (the mission-independent set: vehicles, support, common buildings) during intro/menu, but only if the parse is pure (no GL/VK handles, no static-prop batcher registration; note the batcher registration order issue fixed in ec0e5214a). Risks: shared mutable state inside type objects (refcounts, texture handles), batcher registration order (`registerType` before `finalizeGeometry` changes how props shade), mod-stack invalidation. Expected win: most of the 7.5 s warm parse on the second and later loads, and part of it on the first if pre-parsed in the menu.
