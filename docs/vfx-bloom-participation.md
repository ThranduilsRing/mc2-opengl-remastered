<!-- doc-status: current | reviewed:2026-10-11 -->
# VFX bloom participation (VFX-BLOOM-PARTICIPATION-1)

Track V VFX-payoff opus, slice 4. **Status: no code change — VFX already feed
bloom correctly. This slice is documentation + tuning guidance.**

The original opus plan budgeted a "let emissive VFX feed bloom" code slice.
Recon proved the plumbing already exists end-to-end, so the honest deliverable
is to document *how* it works and *which levers* drive it, not to add a
redundant gate. Adding a second knob (e.g. `vfxBloomBoost`) that aliases the
existing additive-brightness setter would create two sources of truth for one
value, so it was deliberately NOT added (minimal-touch).

## Why VFX already bloom

1. **The scene FBO is HDR unconditionally.** `gosPostProcess::createFBOs`
   allocates the scene colour attachment as `GL_RGBA16F` regardless of any
   gate (`GameOS/gameos/gos_postprocess.cpp:462`). It is NOT created only when
   `MC2_HDR_POST` is on — the float buffer is always there.

2. **GPU particles draw into that HDR buffer, before post.** The particle
   bridge flush (`gos_particle_bridge_flush`) runs during the in-scene render
   while the scene FBO is bound, i.e. after opaque geometry and before
   `endScene` / post. So particle pixels land in `sceneColorTex_`.

3. **Bloom bright-passes that same buffer.** `gosPostProcess::runBloom`
   (`gos_postprocess.cpp:807`) binds `sceneColorTex_` (`:831`) and extracts
   pixels above `bloomThreshold_` (`:827`, default **1.2** — tuned up from 0.6
   on 2026-05-29, `:115`). Any particle pixel whose luminance crosses the
   threshold contributes to bloom.

4. **No clamp blocks it.** The particle FS writes `outColor` straight to the
   RGBA16F target with no `clamp`/`saturate` on the scene->bloom path; the only
   `clamp(...,0,1)` lives inside the ACES tonemap (`postprocess.frag`), which
   runs in the composite *after* bloom extract. Additive groups
   (`glBlendFunc(GL_SRC_ALPHA, GL_ONE)`, `gos_particle_bridge.cpp`) accumulate
   past 1.0 in the float buffer, which is exactly what bloom feeds on.

Bloom is only visible when `MC2_HDR_POST=1` AND `MC2_BLOOM=1` (bloom
auto-depends on the HDR master gate). With HDR off, the RGBA16F buffer still
exists but the bloom add and tonemap are skipped, so output is unchanged.

## Levers (all already shipped, default no-op)

| Lever | Where | Default | Effect |
|---|---|---|---|
| `MC2_TUNE_VFX_ADDITIVE_BRIGHTNESS` / profile `vfxAdditiveBrightness` / ImGui "Additive brightness" | `gos_particle_bridge` `u_vfxAdditiveBrightness` (clamp 0..8) | 1.0 | RGB scale on **additive** groups only — the highest-value lever for pushing flashes/PPC/explosions over the bloom threshold |
| `MC2_TUNE_VFX_BRIGHTNESS` / profile `vfxBrightness` | `u_vfxBrightness` (all particles) | 1.0 | global RGB scale |
| per-mission `bloomThreshold` | `visual_tuning_profile` -> `gos_SetBloomThreshold` | 1.2 | luminance cut for the bright-pass; lower = more pixels bloom |
| per-mission `bloomIntensity` | `gos_SetBloomIntensity` | 0.15 | bloom add strength in composite |

To make **sparse, single** muzzle flashes bloom (a lone additive sprite peaks
near luminance ~1.0-1.5 and may sit at/under the 1.2 threshold): raise
`vfxAdditiveBrightness` for that mission (e.g. 1.5-2.0), or nudge that mission's
`bloomThreshold` down toward ~1.0. **Dense** additive effects already cross the
threshold via accumulation and need no help.

## Caveats

- **Do not globally lower `bloomThreshold`.** It was deliberately doubled to 1.2
  (`GameOS/gameos/gos_postprocess_gates.cpp:69`) because stock bloom was too strong; lowering it
  globally washes terrain/skies. Tune per-mission instead.
- **Additive accumulation can saturate to white.** Dense overlapping additive
  draws ramp toward white over ~1s in the float buffer (`gos_postprocess.cpp:
  1177-1179`, the pylon-generator canary on mc2_05/mc2_24). Keep
  `vfxAdditiveBrightness` conservative; over-bright + bloom = blowout.

## Capture verification

Confirm VFX bloom on a VFX-heavy mission with the HDR stack on:

```powershell
$env:MC2_HDR_POST = "1"; $env:MC2_BLOOM = "1"
py -3 scripts/run_smoke.py --mission mc2_24 --mission mc2_10 --duration 30 --keep-logs
```

Compare against the same missions with bloom OFF (unset both). Watch for
muzzle-flash / PPC / explosion halos; verify terrain/sky are not hazed. The
full payoff capture matrix lives in `docs/vfx-payoff-capture-matrix.md`
(VFX-PAYOFF-CAPTURE-MATRIX-1).
