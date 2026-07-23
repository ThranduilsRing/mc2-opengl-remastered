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

## Maintainer guidance: public redistribution of the combined work

> This is maintainer guidance summarizing the license texts in this repo. It
> is **not legal advice**; when in doubt, read [EULA.txt](EULA.txt) and
> [license.txt](license.txt) directly.

What the Microsoft Shared Source Limited Permissive License (EULA.txt)
grants and requires, stated conservatively:

- **Code**: §2(A) grants a royalty-free license to reproduce the software,
  prepare derivative works, and distribute the software or derivative works.
  This is what permits this repo and its release builds to exist publicly.
- **Art Assets**: §3(A) is the key restriction — there are **no rights to
  commercially distribute any artwork from the game**; Art Assets may be
  used, reproduced, and distributed on a **non-commercial basis only**. The
  release gamedata/art/movies zips contain Art Assets, so all public releases
  of this project must remain non-commercial (no selling, no paywalling).
- **Trademarks**: §3(B) grants no rights to Microsoft's name, logo, or
  trademarks — do not brand distributions in a way that implies Microsoft
  endorsement.
- **Notices**: §3(D) requires retaining all copyright, patent, trademark,
  and attribution notices present in the software.
- **Source vs. binary**: §3(E) — source-form distribution of the covered code
  is permitted **only under this same license, with a complete copy of the
  license included** (this is why EULA.txt is in the repo and in every
  release zip); compiled/object-form distribution must be under a license
  that complies with this license.
- **Warranty**: §3(F) — as-is, no warranties.

Interaction with the other buckets: the alariq/mc2 platform-layer files are
GPLv3 (license.txt must accompany distributions containing them), and per the
table above the effective terms of the combined work are governed by the most
restrictive applicable component. Practical net for maintainers: public,
free-of-charge distribution of source and release zips with EULA.txt +
license.txt included and notices intact is what the license texts
contemplate; any commercial distribution is off the table because of the Art
Assets clause.
