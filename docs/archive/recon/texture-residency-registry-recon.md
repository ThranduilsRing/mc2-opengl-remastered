# Texture Residency Registry — Recon (claude/texture-residency-registry-recon-1)

**Recon only. No engine code, no asset conversion, no deploy deletion.**

## Problem

KTX2/BC7 packing + deploy-slimming is per-consumer whack-a-mole. Every subsystem
decides "texture truth" differently, so each KTX win (and the slim deploy that
broke buildings + vehicle textures) is a one-off. We want ONE canonical resolver
answering, per logical texture: *what runtime payload exists? does this consumer
need CPU pixels / GPU sample / array packing / CPU alpha? what fallback is legal?*
— instead of `does foo.tga exist?`.

> **Correction to inherited assumption:** `MC2_TEXMGR_KTX_PRIMARY` is **default
> mode 1 (fallback-on)** (txmmgr.cpp:3616-3619), not off — the in-code comment
> string is stale. Design around default-on ktx2 fallback.

## Residency classes — ship SIX (4 collapses two real contracts)

The discriminator is not "GPU vs CPU" — it's **what payload must physically
survive a slim deploy for this consumer to not crash/vanish**.

| Class | GPU payload | CPU payload | Slim-deploy invariant |
|---|---|---|---|
| `GPU_SAMPLE_ONLY` | bc7 ktx2 OR tga | none | one of {ktx2, tga} exists |
| `CPU_RGBA_REQUIRED` | any | rgba8, lockable | a CPU-decodable source must exist; BC7-GPU-only is **illegal** |
| `GPU_ARRAY_PLUS_CPU_ALPHA` | array layer (ktx2 or tga) | raw alpha retained | **both** array payload AND alpha-bearing source |
| `GPU_TILES_PLUS_CPU_FULL_ATLAS` | tiled GPU (jpg/tga) | full atlas retained (HSV classify) | a CPU-retainable source; ktx2-only variant is a **different** class |
| `LOAD_TGA_THEN_GPU_KTX` | bc7 ktx2 array at draw | none at draw | **tga HARD-REQUIRED at mission load** even though ktx2 is drawn |
| `LEGACY_TGA_ONLY` | tga | tga (locked) | tga only; no ktx2 ever |

The two non-obvious classes are load-bearing:
- **`GPU_TILES_PLUS_CPU_FULL_ATLAS`** (colormap): the `.burnin.jpg`/`.tga` path
  retains `cpuColorMap` for HSV terrain-type classification; the `.burnin.ktx2`
  variant retains nothing. Slimming one to ktx2 silently breaks terrain typing —
  they are not the same residency object.
- **`LOAD_TGA_THEN_GPU_KTX`** (static-prop batcher): the most dangerous consumer.
  The tga is a **load-time** dependency (node must exist or `nodeIdx=0xFFFFFFFF`
  → packet skipped → buildings vanish), even though the `.ktx2` is what's drawn.
  Folding this into `GPU_SAMPLE_ONLY` *is* the whack-a-mole. Keep it distinct.

## Consumer inventory (verified, file:line)

| Consumer | Class | hard `.tga`? | Evidence |
|---|---|---|---|
| common loader `loadTexture` | (routes per class) | no (route-2) | mclib/txmmgr.cpp:3528 |
| BC7 ktx2 sidecar GPU upload | `GPU_SAMPLE_ONLY` | no | txmmgr.cpp:~3888, gate `MC2_TEXMGR_COMPRESSED_UPLOAD` (excludes `uniqueInstance!=0` @3975) |
| route-2 (ktx2→RGBA8 MEM_RAW) | bridge | no | txmmgr.cpp:3620-3666 (BGRA swizzle) |
| `gosASSERT(open==NO_ERR)` | — | **landmine** | txmmgr.cpp:3669 — fires if tga gone AND no ktx2 |
| mech paint scheme | `CPU_RGBA_REQUIRED` | yes | mech3d.cpp:1825 `gos_LockTexture` rewrites R/G/B mask; loaded with `paintInstance` (uniqueInstance!=0) @2013 |
| vehicle paint scheme | `CPU_RGBA_REQUIRED` | yes | gvactor.cpp:1220 |
| UI StaticInfo (dims+readback) | `CPU_RGBA_REQUIRED` | yes | utilities.cpp:226/318 (hardcoded `.tga` @297) |
| control GUI button atlas | `CPU_RGBA_REQUIRED` | yes | controlgui.cpp:3002 |
| effect 8→32-bit LUT | `CPU_RGBA_REQUIRED` | yes | cevfx.cpp:551 |
| **GPU static-prop batcher** | `LOAD_TGA_THEN_GPU_KTX` | **yes @load** | gos_static_prop_batcher.cpp:1005 (getTextureName→tga), `MC2_MATERIAL_KTX` @669, glCompressedTexSubImage3D; RGBA8 fallback via glGetTexImage of the tga-loaded texture |
| **terrain PBR splat array** | `GPU_ARRAY_PLUS_CPU_ALPHA` | **yes (direct)** | terrtxm2.cpp:2415-2510 direct `TGAFileHeader`/`UNC_TRUE` decode → `gos_CreateTerrainNormalTexture` GL array; `cpuDispAlpha` retained @2506 for elevation displacement; bypasses loader+sidecar |
| terrain detail/water/overlay/mine | `GPU_SAMPLE_ONLY` | no | terrtxm2.cpp:1191/1829, terrtxm.cpp:446/459/472, quad.cpp:684 — via `mcTextureManager->loadTexture` |
| burnin colormap jpg/tga | `GPU_TILES_PLUS_CPU_FULL_ATLAS` | yes | terrtxm2.cpp:~1926 (stb decode) + tiles via `textureFromMemoryRaw`; `cpuColorMap` retained |
| burnin colormap ktx2 | `GPU_SAMPLE_ONLY` | no | gos_terrain_indirect.cpp:~873 BuildColormapAtlas, `MC2_COLORMAP_KTX2` default-on, no CPU retain |

**Name conventions that identify a class:** `*rgb.tga` (mech/vehicle) = paint =
`CPU_RGBA_REQUIRED`; `data/art/*` = UI = `CPU_RGBA_REQUIRED`/`LEGACY_TGA_ONLY`;
`mat[0-4]_(normal|displacement).tga` = `GPU_ARRAY_PLUS_CPU_ALPHA`; `*.burnin.*` =
colormap.

## Architecture

- **Build-time manifest is authoritative; runtime resolver is a thin read-only
  reader.** MC2 already does multi-source resolution (loose → base-strip →
  fastfile, txmmgr.cpp:3601); a runtime resolver that re-probes the filesystem
  rebuilds the whack-a-mole. The cook emits a manifest (sparse — only constrained
  textures); `TextureResidency::resolve(logicalName)` returns the record or the
  `GPU_SAMPLE_ONLY` default.
- **Plug in ABOVE `loadTexture`** as a *decision*, not below as another probe.
  The registry's first win is **deleting the reachability of the `:3669` assert**
  by knowing in advance the tga is legitimately absent. txmmgr keeps doing the GL
  mechanics (MEM_RAW build, `glCompressedTexImage2D`); the registry decides which.
- **Subsume** `MC2_TEXMGR_KTX_PRIMARY` + the `if(open fails) try ktx2 else ASSERT`
  heuristic (they become registry-selected backends; keep the env as debug
  override). **Keep as capability/mechanism gates** `MC2_TEXMGR_COMPRESSED_UPLOAD`
  (BPTC device cap, runtime), `MC2_MATERIAL_KTX`, `MC2_COLORMAP_KTX2` — the
  resolver *selects* them.

## Keystone fix — static-prop node-from-ktx2

The slim-deploy "buildings vanish" is because the batcher's node must be loaded
from the tga at mission load (else `nodeIdx=0xFFFFFFFF`). Route-2 already proves a
`.ktx2` can stand alone for the **MEM_RAW** path. Extend that so the batcher's
node is valid from the ktx2 alone:
1. Create the node from the ktx2 header (dims) when tga absent → `nodeIdx` valid.
2. Draw path unchanged (ktx2-at-draw via `glCompressedTexSubImage3D`).
3. Re-source the RGBA8 fallback (today `glGetTexImage` of the tga texture) from the
   same `ktxDecodeBc7ToRgba8` route-2 runs — the decoder is already in txmmgr.

Do NOT drop static-prop tgas in a slim deploy until step 1 lands.

## Integration order

1. Resolver + manifest reader, **advisory/shadow** alongside the current heuristic;
   log mismatches (build trust via dual-output parity before cutover).
2. Actor / `fileExists` gates consult the resolver (pure read, lowest risk).
3. **Static-prop node-from-ktx2** (keystone) — before the slimmer can drop those tgas.
4. Deploy slimmer consults registry — only after 3.
5. Terrain splat `GPU_ARRAY_PLUS_CPU_ALPHA` support — last (biggest mechanism change).

## Validator invariants (cook-time, FAIL the build)

1. `CPU_RGBA_REQUIRED` with no CPU-decodable source (every `gos_LockTexture` site).
2. `GPU_ARRAY_PLUS_CPU_ALPHA` missing either the array payload or the alpha source
   (and `width==arrayWidth`, the code's own check @terrtxm2.cpp:2469).
3. `LOAD_TGA_THEN_GPU_KTX` with no loadable node source (tga required pre-keystone;
   ktx2+node-path required post-keystone).
4. `GPU_TILES_PLUS_CPU_FULL_ATLAS` shipped ktx2-only while a CPU-atlas consumer exists.
5. Any registry-known-tga-absent texture whose loader route still reaches `:3669`.
6. `consumers[]` inconsistent with class (same texture both `gos_LockTexture`d and
   marked BC7-GPU-only = contradiction).

## Smallest viable v1 (no giant DB)

A **sparse** manifest listing only the constrained sets — the entire risk surface —
everything else defaults to `GPU_SAMPLE_ONLY` with no entry:
1. Cook pass emits records for: the 6 `gos_LockTexture` sets (`CPU_RGBA_REQUIRED`),
   the static-prop set (`LOAD_TGA_THEN_GPU_KTX`), terrain splat (`GPU_ARRAY_PLUS_CPU_ALPHA`).
2. `TextureResidency::resolve()` → record or `GPU_SAMPLE_ONLY` default.
3. Static-prop node-from-ktx2 (keystone step 1) — unblocks the largest tga tonnage.
4. Validator invariants #1 + #3 only.

Defer to v2: terrain `GPU_ARRAY` resolver integration, colormap class handling,
full consumer graph, subsuming `MC2_TEXMGR_KTX_PRIMARY`.

## Open risk / next step

The keystone (static-prop node-from-ktx2) touches the GL readback fallback — it
**must be validated with a tier1 smoke on a slim deploy (static-prop tgas dropped)
+ a visual check** (the flythrough smoke missed the vanish before). Static analysis
can't confirm runtime. Schema + sample entries: `docs/texture-residency-registry.schema.yaml`.
