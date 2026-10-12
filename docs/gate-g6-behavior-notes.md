# G6 (mclib family) -- behaviour notes and corrections

The generated plan docs/gate-codemod-g6.md (deleted with its generator tools/gate_codemod.py, retired 2026-10-12, cull wave 1; archived on E:/mc2-archive-2026-10/docs/cull-wave1-4-2026-10-12.tar.gz) was written before the rewrite and was never regenerated (the sources it
describes are typed now). This file records the input changes the review found that the plan and the commit messages
do not say, and corrects two statements. No harness (scripts/, tests/, tools/, data/, launcher_env.json, the smoke
matrices) sets any of these inputs.

## Exact-"0" opt-outs that became leading-"0" (G0 ruling), not listed before

| Gate | Site | Old | Now |
|---|---|---|---|
| `MC2_ABL_COMPILE_FATAL` | `mclib/ablrtn.cpp` (`s_compileFatal`) | only `0` disables | any value starting with `0` disables |
| `MC2_LIGHT_POS_COLUMN_FIX` | `mclib/txmmgr.cpp` (`s_lightPosColumnFix`) | only `0` disables | any value starting with `0` disables |
| `MC2_TERRAIN_INDIRECT`, `MC2_TERRAIN_INDIRECT_OVERLAY` | `mclib/terrtxm2.cpp` (`s_skipColormapTiles` cache) | only `0` disables | any value starting with `0` disables |

The tool now reports this class: a Bool site that tests for an exact `"0"` (`v[1] == '\0'`, `strcmp(v, "0")`) against a
leading-`0` row is an EDGE (label `X01`, input `01`/`00`/`0x`), not NO-CHANGE.

## Level-valued gates (Enum rows)

* `MC2_STATIC_PROP_BUILDING_SHADOW`: an **empty** value used to read as on (set, first character not `0`); it now reads
  as the default level `0` (off). All three readers (bdactor, both txmmgr sites) agree.
* `MC2_STATIC_PROP_BUILDING_SHADOW_REBAKE` and `MC2_SHADOW_DYNAMIC_PROP_CASTERS` (default ON, `=0` kill): `00`, `0.5`,
  `0x` and other values starting with `0` used to kill; they are unknown tokens now and read the default (ON). The
  `d8a822f32` note that unknown text "keeps the default ON, as before" is wrong for these inputs.
* Trace levels: `2x`, `20` and other values that merely start with `2` no longer enable the trace line; only `2` does.
* Tokens are matched case-insensitively and exactly (the admission-predicate enums, `MC2_SPOTLIGHT_DIRECTION_FIX`).
  The documented spellings (`modern`, `Modern`, `Bypass`) are tokens of their rows.

## Strict mode

Under `MC2_CONFIG_STRICT=1` a Std Bool row reports `badvalue` for text outside 0/false/off/no/1/true/on/yes. The
`MC2_STABLE_LIGHT_SKIP*` / `MC2_LIGHTBRIDGE_STABLE_SKIP*` / `MC2_FRAME_JOBS_TOUCH_DIAG` family and
`MC2_MODERN_TEX_RESOLVE` are Std rows, so `=2` (read as on, as before) is now fatal under strict. The row docs say so.

## NaN

`atof("nan")` is NaN in the old readers and compared false against every bound; a typed Float row rejects NaN and reads
its default. Affected: `MC2_CAMERA_SMOOTH`, the seven cone_glow knobs (`MC2_CONE_POOL_REACH`, `MC2_LIGHT_CONE_GLOW_ALPHA`,
`MC2_LIGHT_CONE_SPILL`, `MC2_LIGHT_REALISM_CONE_OPACITY`, `_CONE_FEATHER`, `_BEACON`, `_BEACON_RADIUS`),
`MC2_GLTF_YOFF`, `MC2_IMPOSTOR_DIST`, `MC2_LOWCAM_OBJ_NEARPAD_SCALE`, `MC2_MECH_IMPORT_GPU_LIFT`,
`MC2_TERRAIN_DECAL_*`, `MC2_MECH_SKEL_HEIGHT`.

## Correction: 1e999

The commit notes list `1e999` (infinity) as changing `MC2_GLTF_YOFF`, `MC2_IMPOSTOR_DIST` and
`MC2_LIGHT_POOL_MOVER_FWD`. It does not: Prefix rows pass infinity through, the first two have no range (the old reader
kept infinity too) and the third is a clamp row (the old reader clamped). Only rows with a Reject range
(`MC2_SHADOW_CASTER_CULL_MARGIN`) change on `1e999`.
