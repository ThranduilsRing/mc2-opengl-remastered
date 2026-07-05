# License provenance

This repository contains code from three distinct origins. No single license
covers the whole tree; the effective license of the combined work is governed
by the most restrictive applicable component.

| Bucket | What | License |
|---|---|---|
| Microsoft MechCommander 2 shared source (2002 release) | original `code/`, `mclib/`, `gui/`, `GameOS/` game code and derived files | Microsoft Shared Source Limited Permissive License — full text in [EULA.txt](EULA.txt) |
| alariq/mc2 port scaffolding (2016–2021 fork base) | GL/SDL platform layer: `GameOS/src/platform_*.cpp`, `GameOS/gameos/utils/`, parts of `GameOS/gameos/gameos_graphics.cpp`, `gos_render.cpp`, `gameos_sound.cpp` | GPLv3 — full text in [license.txt](license.txt) |
| Remaster code (2025–) | `RenderCore/`, `RenderWorld/`, `GameAdapters/`, `shaders/` (modern), `tools/`, `tests/`, and the majority of modified engine lines | MIT (where marked) |
| Third-party | `3rdparty/` and vendored libs | their own licenses |

A machine-generated provenance audit (file buckets + per-line attribution vs
the alariq/mc2 fork base) is kept under `.claude/provenance/` on the source
repository.

New contributions are MIT unless they modify a file from the first two
buckets, in which case they inherit that file's license.
