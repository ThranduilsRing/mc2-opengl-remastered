# SHORELINE-FOG-NEXT-SLICES — opus-ready slice queue

Follow-on work after TERRAIN-SHORELINE-V3 horizontal-native defaults (`80d19d1c`) and
SKYBOX-FOG-EXCLUDE-2 fog-wall fix (`d918cd90`), branch `claude/controlmap-sample-1`.
Each slice below is self-contained: goal, exact seams, gates, validation. All default-OFF
or default-value-preserving unless stated. Read `docs/critical_inline_rules.md` first;
shaders deploy in lockstep with the exe; run `scripts/slice_gate.py` per slice.

## Current state (contract you inherit)

- Band placement: `shaders/terrain_lod_chunk.frag` main() shoreline block (~line 1000-1135).
  Placement = `v_worldPos.z` vs `u_waterElevation` (elevation gate), width measured as
  HORIZONTAL run from the drawn waterline via macro-slope conversion
  (`run = aboveWater * Nz/sinSlope`, slope clamped >= ~1.7deg).
- Defaults: wet 16wu (~4.8m) run, foam 5wu (~1.5m). Knobs `MC2_TERRAIN_SHORELINE_WET_RUN`
  / `_FOAM_RUN` (legacy `_WET_HEIGHT`/`_FOAM_HEIGHT` = aliases, same horizontal units now).
  Intensity: `MC2_TERRAIN_SHORELINE_STRENGTH` / `_FOAM` [0,2].
- Gate: `MC2_TERRAIN_SHORELINE` default OFF (byte-identical OFF). Mask sidecar OPTIONAL
  modulator: `data/missions/<stem>.beauty/shoreline_mask.png` (G=wet, B=foam weights),
  cooked by `tools/terrain_beautify/cook_shoreline.py`; band works without it.
- Slope guard: full <12deg, zero >20deg (`kSlopeFullCos`/`kSlopeZeroCos` in frag) — KEPT
  deliberately as the sole cliff-face suppression (run measure normalizes plan view only;
  a 16wu run at 45deg still climbs 16wu vertically up a bank).
- Fog: `MC2_SKYBOX_FOG_EXCLUDE` default ON; stencil tag = elevation `worldDir.y > 0.22`
  (`shaders/hdri_skybox_stencil_tag.frag`); fog side feathers by the shared
  `smoothstep(-0.22,-0.01, worldDir.z)` band (`fog_oob.frag`, `edge_fog.frag`).
  `shaders/vulkan/{fog_oob,edge_fog}.frag` carry a KNOWN-DIVERGENCE note (feature not
  ported to islands; port the FEATHERED v2 form only, never v1 hard zero).

---

## 1. SHORELINE-BATCH-COOK-1 — cook masks for all water missions

**Goal:** every mission with water gets a `shoreline_mask.png` sidecar so wide-beach
falloff / basin exclusion works everywhere, not just hand-cooked maps.

- Add `--all-missions` batch mode to `tools/terrain_beautify/cook_shoreline.py`
  (single-mission CLI exists; water detection via
  `mission_terrain_analyzer.read_water_elevation` — NEVER the PostcompVertex `.water`
  byte, it's packed alpha not a bool, analyzer:178-186).
- Skip missions where `waterElevation` yields zero water cells; emit a cook census
  (mission, water %, mask path, band stats) like WHOLESALE-VECTORIZE-1's census pattern.
- Wire sidecars into `scripts/deploy_payload.py` payload set if `.beauty/` dirs are not
  already included (verify first — grep deploy manifest).
- **Validate:** pytest `tools/terrain_beautify/test_cook_shoreline.py` extended with a
  batch fixture; spot-run 2 water missions (mc2_17 + one other) gate-ON, confirm no
  `[SHORELINE]` load errors in console; slice_gate smoke pair.
- Size: S-M (tooling only, zero engine code).

## 2. SHORELINE-WET-DARKEN-TUNE-1 — wet look beyond flat 0.86 darken

**Goal:** wet band currently `lit * 0.86` mix (frag ~line 1114-1116) — reads as a grey
smudge on bright sand. Real wet ground darkens MORE and saturates.

- Replace flat darken with albedo-space wet response: `wetAlbedo = pow(albedo, 1.3-1.6)`
  style saturation-preserving darken (tunable exponent), optional slight blue-green tint
  near the waterline. Keep `u_shorelineStrength` as the master mix.
- Consider a specular/gloss kick if the LOD-chunk path exposes roughness (check the PBR
  uniforms in `terrain_lod_chunk.frag` before promising it; if not exposed, ship
  albedo-only and note the seam).
- Knob: `MC2_TERRAIN_SHORELINE_WET_EXP` (default = current-look equivalent), keep default
  visually close to today's 0.86 mix so cooked screenshots don't all shift.
- **Validate:** gate-ON screenshot pair (mc2_17 shoreline bookmark) before/after;
  slice_gate smoke pair. cement/concrete exclusion (`pureConcrete`) must stay.
- Size: S (frag + 1 uniform + env knob + docs/registry).

## 3. SHORELINE-FOAM-STYLE-1 — foam styling options

**Goal:** foam is fbm wisps (`foamScroll`/`foamPulse`, frag ~line 1124-1132); offer
styled variants for art direction.

- Variant A (banding): waterline-parallel phase stripes — modulate coverage by
  `sin(horizDist * k - u_shaderTime * speed)` so foam arrives in bands like lapping waves.
  `horizDist` already computed; zero new inputs.
- Variant B (advance/retreat): oscillate the effective foam run
  `foamHeight * (0.8 + 0.2*sin(u_shaderTime * w))` — the waterline edge breathes.
- Style selector: `MC2_TERRAIN_SHORELINE_FOAM_STYLE=0|1|2` (0 = current wisps, default).
  Uniform int, C++ side mirrors the existing knob pattern in
  `GameOS/gameos/gos_terrain_lod_chunk.cpp` (~line 1340-1385).
- IMPORTANT: keep everything `f(worldPos, u_shaderTime)` only — `u_shaderTime` is the
  camera-independent clock (SmokeMode-fixed); no camera-dependent animation.
- **Validate:** slice_gate smoke pair; visual check all 3 styles via env flip (no rebuild).
- Size: S-M (frag + 1 uniform + docs/registry).

## 4. FOG-WALL-POLISH-1 — remaining fog wall / horizon polish

**Goal:** SKYBOX-FOG-EXCLUDE-2 killed the azimuth-wedge artifact; residual polish items:

- Band-top knob: the 0.22 elevation threshold is duplicated in 3 shaders
  (`fog_oob.frag` fade, `edge_fog.frag` feather, `hdri_skybox_stencil_tag.frag` tag).
  Hoist to a shared uniform `u_skyBandTop` (default 0.22, env
  `MC2_SKYBOX_FOG_BAND_TOP`) so the horizon height is tunable and the three stay in
  lockstep by construction. Byte-identical at default.
- Horizon color match: OOB cloud color vs HDRI horizon row can mismatch under
  exposure/grade (FOG-EXPOSURE-HEADROOM-1 `592aed80` fixed blowout, not hue). Option:
  sample the HDRI at horizon elevation for `u_fogColor` when HDRI is active (C++-side
  average, one-time per mission — not per-frame).
- edge_fog lateral shape: `distFromEdge = halfExtent - max(|x|,|y|)` gives square-corner
  fog; consider rounded-corner metric `length(max(abs(planeXY)-halfExtent+r, 0))-r` behind
  a default-OFF gate if corners read as walls at low camera.
- Mirror rule: any change to `fog_oob.frag`/`edge_fog.frag` math must update the
  KNOWN-DIVERGENCE notes in `shaders/vulkan/{fog_oob,edge_fog}.frag` (or port, if the
  Vulkan backend has reached those passes by then).
- **Validate:** slice_gate smoke pair mc2_17 (HDRI mission) + one non-HDRI mission
  (stencil never tagged — fallback band must stay byte-identical); low-camera map-edge
  bookmark screenshots at 4 azimuths.
- Size: M.

## 5. EDGE-BLEND-OVERLAY-1 — map-edge presentation family

**Goal:** unify the map-edge treatments (edge fog wall, OOB sea-of-clouds, terrain edge
feather) into one coherent, art-directable "edge blend" family instead of three
independent effects that can disagree.

- Inventory first (recon slice): `MC2_EDGE_FOG*` (edge_fog.frag), `MC2_OOB_FOG*`
  (fog_oob.frag), `MC2_TERRAIN_EDGE_FEATHER[_STRENGTH]` (terrain_lod_chunk.frag, gate
  cached as `kTglcEdgeFeather` after REDUNDANT-PASS-HUNT-1 `6cc2d08a`), plus the OOB
  void clear color. Document which pixels each owns (terrain edge band / void below
  horizon / sky above) and where seams show today.
- Then a preset knob: `MC2_EDGE_BLEND_PRESET=clouds|haze|vignette|off` that sets the
  family coherently (C++-side env → existing uniforms only; no new shader math in the
  first slice).
- Candidate follow-ups: haze preset (distance-fade terrain into fog color, no cloud
  texture), vignette preset (darken instead of fog for night missions).
- **Validate:** per-preset screenshot set at the map-edge bookmark; slice_gate smoke
  pair per preset value; default preset must be byte-identical to today.
- Size: recon S, preset slice M.

---

## Validation boilerplate (every slice)

```
py -3 scripts/slice_gate.py --no-build --deploy-target "A:/Games/mc2-opengl/mc2-win64-v0.4" \
    --deploy-lane 0.4 --mission mc2_17 --gate <GATE>=1
```

plus `bash scripts/check-env-registry.sh` and `py -3 scripts/check-gate-defaults.py` when
adding env vars, and `docs/tier1_env_vars.md` entries for every new knob. Do NOT deploy
to `0.5-testing` from this lane without coordinator sign-off.
