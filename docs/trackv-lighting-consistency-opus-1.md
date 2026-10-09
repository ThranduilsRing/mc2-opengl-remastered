# TRACKV-LIGHTING-CONSISTENCY-OPUS-1

Cross-lane visual-balance consistency reference for the five mature R->V lanes
(terrain, static props/buildings, water, mechs, VFX) plus shadows and the
HDR/bloom/tonemap/SSAO post stack. Produced after `TRACKV-POST-GROUNDING-MVP`
merged to `claude/nifty-mendeleev` (merge `d8ccd032`, ledger `8d6be64f`).

Branch: `claude/trackv-lighting-consistency-1` (off `claude/nifty-mendeleev`).

Scope: this is a **consistency / tuning / debug / capture** reference. It is NOT
a renderer-architecture document. Core principle for the whole opus: **no
default visual behavior change until reviewed** — use gates, profiles, candidate
values and captures first.

All file:line references were grep-verified at write time against this worktree.
Symbols are stable; line numbers drift — re-grep before relying on a number.

---

## 1. Lane-by-lane lighting model inventory

### Sun / global (NOT a render tunable)

The gameplay sun (direction + color used by terrain/mech/prop lighting) is
**map data**, not a render knob:

- Direction: `eye->lightDirection` (`mclib/mapdata.cpp:703`, `mclib/terrtxm2.cpp:615`).
- Color: packed `startLight` (`mclib/txmmgr.cpp:992-995`).

There is **no env / profile / ImGui override** for sun color, direction or
intensity. The only "sun" the renderer owns is the *decorative skybox* sun color,
hardcoded (`gos_postprocess.cpp:1237-1239`, zenith `{.18,.35,.72}`, horizon
`{.55,.62,.72}`, sun `{.9,.8,.6}`) — unrelated to gameplay lighting. Consequence:
there is currently no lever to warm/cool or re-aim the gameplay sun for
cross-mission consistency. Out of scope for this opus (would be a new feature).

### Full tunable table

| Lane | Tunable | Controls | Mechanism | Location | Default |
|------|---------|----------|-----------|----------|---------|
| Post | Exposure | HDR exposure pre-tonemap | **profile** `exposure` + ImGui | `gos_postprocess.cpp:72-73,110`; `visual_tuning_profile.cpp:141` | `1.0` (mc2_24=`1.1`) |
| Post | HDR master gate | enables whole HDR stack | env `MC2_HDR_POST` | `gos_postprocess.cpp:252-253` | OFF |
| Post | Tonemap (ACES) | ACES tonemap | env `MC2_TONEMAP_ACES` (needs HDR) | `gos_postprocess.cpp:273-275` | OFF |
| Post | Bloom enable | bloom pass | env `MC2_BLOOM` (needs HDR) | `gos_postprocess.cpp:261-263` | OFF |
| Post | Bloom threshold | bright-pass cutoff | **profile** `bloomThreshold` + ImGui | `gos_postprocess.cpp:78-79,84,115`; `vtp.cpp:173` | `1.2` |
| Post | Bloom intensity | composite weight | **profile** `bloomIntensity` + ImGui | `gos_postprocess.cpp:81-82,85,114`; `vtp.cpp:178` | `0.15` |
| Post | SSAO enable | AO pass | env `MC2_SSAO` | `gos_postprocess.cpp:325-326` | OFF |
| Post | AO radius/strength/bias | SSAO params | **profile** `aoRadius`/`aoStrength`/`aoBias` + ImGui | `gos_postprocess.cpp:89-94,156-158`; `vtp.cpp:181-189` | `3.0`/`0.7`/`0.0025` |
| Terrain | Tint strength scale | colormap tint blend | **ImGui-only** | `gameos_graphics.cpp:7731-7736` | `1.0` |
| Terrain | Normals-from-height strength | slope normal contribution | **ImGui-only** | `gameos_graphics.cpp:7738-7743` | `1.0` |
| Terrain | Mat normal boost (rock/grass/dirt/concrete) | per-material normal intensity | **ImGui-only** + GLSL fallback | `gameos_graphics.cpp:7724-7730` | `{0.9,1.1,1.1,2.5}` |
| Static prop | IBL SH strength | SH-L2 ambient | **profile** `staticPropIblStrength` + ImGui + env `MC2_STATIC_PROP_IBL_SH_STRENGTH`, gate `MC2_STATIC_PROP_IBL_SH` | `gos_static_prop_batcher.cpp:203-210,485`; `vtp.cpp:153` | `0.5`, gate **ON** |
| Static prop | PBR V1 specular strength | Schlick-Fresnel spec | env `MC2_STATIC_PROP_PBR_V1_STRENGTH` + ImGui, gate `MC2_STATIC_PROP_PBR_V1` | `gos_static_prop_batcher.cpp:218-230,498` | `0.5`, gate **OFF** |
| Static prop | Ambient V1 | hemisphere ambient | env gate `MC2_STATIC_PROP_AMBIENT_V1` only | `gos_static_prop_batcher.cpp:468-471` | OFF |
| Static prop | SH coefficients | 9 SH-L2 RGB consts | C++/GLSL const | `RenderCore/IblShCoeffs.h:35` | from DaySkyHDRI063B |
| Mech | Ambient V1 strength | hemisphere ambient fill | env `MC2_MECH_AMBIENT_V1_STRENGTH` + ImGui, gate `MC2_MECH_AMBIENT_V1` | `gos_mech_batcher.cpp:184-190`; gate `:180`; getter/setter `:2202-2207` | `0.15`, gate **ON** |
| Mech | Specular V1 strength | Blinn sheen | **profile** `mechSpecularStrength` + env `MC2_MECH_SPECULAR_STRENGTH` + ImGui, gate `MC2_MECH_SPECULAR_V1` (needs ViewUniforms) | `gos_mech_batcher.cpp:200-206`; getter/setter `:2216-2221`; shader `mech.frag:138-172` | `0.05` (was 1.0; tuned default), gate **ON** |
| Mech | Metal/glass roughness | spec lobe width | env `MC2_MECH_METAL_ROUGHNESS`/`_GLASS_ROUGHNESS` | `gos_mech_batcher.cpp:207-220` | `0.85`/`0.25` |
| Water | Sky-tint strength | flat horizon tint | **profile** `waterSkyTintStrength` + ImGui + env `MC2_WATER_SKYTINT` | `gameos_graphics.cpp:2264-2274`; `vtp.cpp:158` | `0.0` (env bumps 0.15) |
| Water | Reflection strength (SH sky) | SH-L2 sky reflection | env `MC2_WATER_REFLECTION` (HARD GATE + bumps 0.15) + ImGui | `gameos_graphics.cpp:2284-2300` | `0.0` |
| Water | RT reflection blend | terrain-RT mirror over SH sky | env `MC2_WATER_REFLECTION_RT` (hard gate) + ImGui | `gameos_graphics.cpp:2306-2320` | `0.0` |
| VFX | Brightness | particle color scale | **profile** `vfxBrightness` + ImGui + env `MC2_TUNE_VFX_BRIGHTNESS` | `gos_particle_bridge.cpp:100,111`; `vtp.cpp:163` | `1.0` |
| VFX | Additive brightness | additive-blend scale | **profile** `vfxAdditiveBrightness` + ImGui + env `MC2_TUNE_VFX_ADDITIVE_BRIGHTNESS` | `gos_particle_bridge.cpp:101,112`; `vtp.cpp:168` | `1.0` |
| VFX | Alpha scale | alpha-blend particle opacity | env `MC2_TUNE_VFX_ALPHA_SCALE` + ImGui | `gos_particle_bridge.cpp:113` | `1.0` |
| Shadow | Softness | PCF penumbra radius | **profile** `shadowSoftness` + ImGui | `gameos_graphics.cpp:7717`; `vtp.cpp:144` | `0.9` |
| Shadow | Bias factor/units | glPolygonOffset depth bias | **ImGui-only** | `gos_postprocess.h:72-73`; `GraphicsOptionsWindow.cpp:1542,1545` | `2.0`/`4.0` |
| Shadow | Master enable | shadow pass | env `MC2_SHADOW_ENABLE` | `gos_static_prop_batcher.cpp:5467` | OFF |
| Shadow | Dynamic prop casters | moving casters | env `MC2_SHADOW_DYNAMIC_PROP_CASTERS` | (under shadow enable) | ON |
| Shadow | Screen-shadow softness (post) | screen-space PCF | **hardcoded `0.9f`** (duplicates softness; drift risk) | `gos_postprocess.cpp:938` | `0.9` |

---

## 2. Profile system (MISSION-VISUAL-TUNING-1)

Files: data `data/visual_tuning.json`; loader `GameOS/gameos/visual_tuning_profile.cpp`
+ `.h`; load call `code/mission.cpp:2405` (keyed on ScenarioScript name, e.g.
`"mc2_24"`); ImGui panel `GuiRuntime/GraphicsOptionsWindow.cpp:1638-1672`; doc
`docs/visual-tuning-profiles.md`.

**Precedence** (engine < profile defaults < profile mission < env < ImGui):

| Tier | Mechanism | Location |
|------|-----------|----------|
| Engine default | member initializers | `gos_postprocess.cpp:110-158`; terrain getters `gameos_graphics.cpp:7749,7756` |
| Profile defaults then mission | `applyProfile` applies `defaults` then `missions.<name>` | `visual_tuning_profile.cpp:262-268` |
| Env (selective) | `envIsSet()` guard skips the profile write | `vtp.cpp:154,159,164,169` |
| ImGui live | last-writer-wins (reset on next mission load) | `GraphicsOptionsWindow.cpp:384,488,518,1459` |

Important: the documented "env always overrides profile" only holds for the four
keys with `envIsSet()` guards (`staticPropIblStrength`, `waterSkyTintStrength`,
`vfxBrightness`, `vfxAdditiveBrightness`). The other keys have no *value* env var
(their envs are on/off gates), so there is no conflict. Two special cases:
- Terrain V1/V2: the profile writes the member strength, but the on/off env gate
  is re-read at upload and force-zeroes/force-floors the uniform when OFF — the
  gate wins over the profile (`gameos_graphics.cpp:5349,5362`).
- A fresh mission load re-applies the profile, resetting any live ImGui edits.

**Missing-key = current behavior — CONFIRMED safe** at three layers: missing file
-> no-op (`vtp.cpp:243-244`); missing mission entry -> only `defaults` applied
(`vtp.cpp:264-268`); missing key in a block -> setter never called, engine member
default stands. Unknown key -> one-time stderr warning, no crash (`vtp.cpp:192`).
The shipped `defaults` block mirrors engine defaults, so applying it is a no-op
except mc2_24 `exposure` 1.0->1.1.

**Schema versioning: not present, not needed yet.** Parser is forward-compatible
(unknown keys warn-and-skip; missing keys fall back to engine). Only needed if a
key's *semantics/units* change — defer until then.

### Profile-backed status

| Field | Backed | Default-when-absent | Location |
|-------|--------|---------------------|----------|
| `exposure` | yes | 1.0 | `vtp.cpp:141` |
| `shadowSoftness` | yes | 0.9 (getter 2.5 fallback) | `vtp.cpp:144` |
| `staticPropIblStrength` | yes (env-guard) | 0.5 | `vtp.cpp:153` |
| `waterSkyTintStrength` | yes (env-guard) | 0.0 | `vtp.cpp:158` |
| `vfxBrightness` | yes (env-guard) | 1.0 | `vtp.cpp:163` |
| `vfxAdditiveBrightness` | yes (env-guard) | 1.0 | `vtp.cpp:168` |
| `bloomThreshold` | yes (reader; NOT in JSON; NOT in writer) | 1.2 | `vtp.cpp:173` |
| `bloomIntensity` | yes (reader; NOT in JSON; NOT in writer) | 0.15 | `vtp.cpp:178` |
| `aoRadius` | yes (reader; NOT in JSON; NOT in writer) | 3.0 | `vtp.cpp:181` |
| `aoStrength` | yes (reader; NOT in JSON; NOT in writer) | 0.7 | `vtp.cpp:186` |
| `aoBias` | yes (reader; NOT in JSON; NOT in writer) | 0.0025 | `vtp.cpp:189` |

**Reader/writer asymmetry (real bug):** `applyKey` handles 13 keys, but
`visualTuning_saveCurrentToMission` (`vtp.cpp:285-292`) snapshots only the
original 8 — bloom/AO are silently dropped when "Set as Mission Defaults" is
clicked. Any new profile key must be added to BOTH paths or it will not
round-trip. (Addressed in Slice 2.)

### Gaps — env/ImGui-only that should become profile-backed

Ranked by cross-lane consistency value:

1. **Mech ambient + specular** (`gos_mech_batcher.cpp:184,200`) — no profile key
   at all. Biggest gap: terrain/prop/water ambient are all profile-backed but the
   mech lane has zero profile presence, and both gates are **default-ON** so the
   look is live. Getters+setters already exist (`batcher_get/setMechAmbientStrength`,
   `..SpecularStrength`). Clean env-guard on `MC2_MECH_AMBIENT_V1_STRENGTH` /
   `MC2_MECH_SPECULAR_STRENGTH`. -> **Slice 2.**
2. **Static-prop PBR V1 specular strength** (`g_pbrV1Strength`) — env+ImGui only
   while the sibling IBL strength is profile-backed. Gate default-OFF, so lower
   urgency. (Setter exists? verify before wiring.)
3. **Water reflection strength** (`g_waterReflStrength`) — env+ImGui only while
   the sibling `waterSkyTintStrength` is backed. CAVEAT: `MC2_WATER_REFLECTION`
   is a *hard gate* (`gos_GetWaterReflectionGate`, `gameos_graphics.cpp:2296`) —
   when off, reflection is not applied at all, so a profile strength is inert; when
   on (env set), the env-guard skips the profile. Profile-backing it is therefore
   largely ineffective unless the gate is decoupled from the strength. **Defer.**
4. **Shadow bias factor/units** (`gos_postprocess.h:72-73`) — ImGui-only, no env,
   no getter for snapshot. `shadowSoftness` is backed but bias is not. Would need
   new getters/setters. **Defer.**
5. **Terrain tint-scale / normals-from-height / mat-normal-boost**
   (`gameos_graphics.cpp:7724-7743`) — ImGui-only; materially change terrain look
   but cannot be pinned per-mission. **Defer.**
6. **Screen-shadow softness literal** (`gos_postprocess.cpp:938`) — hardcoded
   `0.9f` duplicating the softness default; will drift if `shadowSoftness`
   changes. Fixing it to read `gos_GetTerrainShadowSoftness()` IS a behavior change
   (it would start tracking ImGui edits) so it is **do-not-touch** without review.

---

## 3. Debug / inspection views

Canonical registry: `RenderCore/RenderDebugView.h` (`RenderDebugView` enum 7-19;
per-lane masks 23-54). Note `kDebugViewMask_Terrain = 0u` and
`kDebugViewMask_Shadow = 0u` (lines 53-54) — those lanes are NOT in the canonical
vocabulary even though their shaders have rich ad-hoc modes.

| Lane | Debug views available | Mechanism |
|------|----------------------|-----------|
| Terrain | Depth(1), Raw Colormap(2)~albedo, Blurred(3), Mat Weights(4), Normal Lighting(5)~lighting-only, Shadow Factor(6), Cloud Shadow(7), Cement(8), Thin-Record(9), Height Normal(10)~normal, Hemi Additive(11) | ImGui combo + hotkeys (RAlt+0, Alt+1); `gos_Set/GetTerrainDebugMode`; shader `gos_terrain.frag` |
| Static prop | (A) Albedo(1)/MaterialIdx(2)/Normal(3)/TexArrayLayer(4)/Roughness(5)/Metallic(6) via `u_debugMaterialMode`; (B) addressing modes 0-7 | Inspector combo (RenderDebugView) + env `MC2_STATIC_PROP_DEBUG_MATERIAL`; `static_prop.frag` |
| Water | Tint(1)~albedo, Alpha(2), Normal(3 flat-up stub), Depth(4), Shore(5), Lighting(6), Reflection/SH-sky(7), RT(8), Blend(9) | ImGui combo + env `MC2_WATER_DEBUG_MODE`; `gos_terrain_water_mdi.frag` |
| Mech | Final(0)/Albedo(2)/LightingOnly(3)/Normal(4)/UV(5)/... (1-9) | Inspector combo (RenderDebugView: Final/Albedo/Normal/LightingOnly) + env `MC2_MECH_FRAG_DEBUG`; `mech.frag` |
| VFX | Final(0)/Albedo(1)/Alpha(2)/ParticleKind(3)/Overdraw(4) + brightness/additive/opacity sliders | ImGui combo + env `MC2_VFX_DEBUG_MODE`; `particle_billboard.frag` |
| Shadow | static/dynamic map select; screen-shadow Normal/Visualize | ImGui radios; `shadow_screen.frag` |
| Post/SSAO | AO buffer grayscale | ImGui "Debug: show AO buffer"; env `MC2_SSAO_DEBUG`; `ssao_apply.frag` |

Canonical-view coverage (what exists today):

| View | Terrain | Prop | Water | Mech | VFX | SSAO |
|------|---------|------|-------|------|-----|------|
| Albedo | ~(2) | yes | ~(1) | yes | yes | - |
| Normal | yes(10) | yes | stub | yes | - | - |
| Lighting-only | ~(5) | NO | ~(6) | yes | - | - |
| IBL-only | NO | NO | ~(7) | NO | - | - |
| Specular-only | NO | NO | NO | NO | - | - |
| AO | NO | NO | NO | NO | - | yes |

**Gaps that would block tuning:** no IBL-only or specular-only on ANY opaque lane
(prop IBL/spec math is vertex-stage in `static_prop.vert`, so a per-fragment debug
branch is *new plumbing*, not near-present). Terrain has Albedo/Normal/
LightingOnly/Shadow modes but is outside the canonical registry, so a cross-lane
"show X everywhere" sweep can't include it through one vocabulary.

**Smallest useful addition (deferred to Slice 5, nice-to-have):** promote the
existing terrain modes into the canonical registry — flip `kDebugViewMask_Terrain`
non-zero + add a `TerrainViewToShaderMode` mapping (pattern already exists for
static-prop and mech) + one inspector combo. No shader edits, no new uniforms.
Strictly higher-leverage and lower-risk than attempting IBL-only/specular-only.

---

## 4. Capture matrix (Slice 3)

Capture tool: `scripts/quick_shot.py`. Usage:
`py -3 scripts\quick_shot.py <MISSION> <WAIT_SEC> <LABEL>`. It copies the parent
shell env first (`os.environ.copy()`), so `$env:MC2_*` set before launch reaches
the engine. All 5 post-stack gates are read once at init via `getenv` (`!='0'`),
so they ARE env-automatable.

Output: `<...>/tests/smoke/artifacts/diag-shots/<MISSION>-<LABEL>.png`.

### Caveats (read before capturing)

- **quick_shot.py hardcodes the v0.3 EXE** (`quick_shot.py:8`) and a
  **nifty-mendeleev output dir** (`:9`). The documented deploy target is
  **v0.4**. To capture a Track-V build, either edit `quick_shot.py:8` or use the
  direct-launch recipe below.
- **`run_smoke.py` cannot carry the 5 post-stack gates** — they are absent from
  its `MC2_*` env-forwarding allowlist (`run_smoke.py:284-523`) and `Popen(env=)`
  replaces the inherited env. Smoke records the default-OFF path regardless of
  shell env. Use `quick_shot.py` or direct launch for gate-ON captures.
- **Shadow debug/tuning is ImGui/hotkey-only** (`docs/shadow-soak-1.md:20-21`) —
  no env path. Needs `MC2_IMGUI=1` build + a longer wait to press the hotkey.
- quick_shot screenshots the **whole desktop** (pyautogui), so the mc2 window
  must be foregrounded — risky in interactive sessions (can grab the wrong
  window). Prefer headless/automated runs.
- **Never `--kill-existing`** on run_smoke (false crash_silent); never `cp -r` on
  deploy; shaders deploy in lockstep with the exe (SSAO needs `ssao.frag` +
  `ssao_apply.frag` or programs fail to compile).

### Config x mission matrix

Missions: `mc2_03` (terrain-heavy), `mc2_17` (mixed), `mc2_24` (mech-heavy).
PowerShell, 28s settle:

```powershell
# default OFF (regression baseline; expect ~byte-identical to shipped)
py -3 scripts\quick_shot.py mc2_03 28 tv-default
py -3 scripts\quick_shot.py mc2_17 28 tv-default
py -3 scripts\quick_shot.py mc2_24 28 tv-default

# HDR only
$env:MC2_HDR_POST="1"
py -3 scripts\quick_shot.py mc2_03 28 tv-hdr; py -3 scripts\quick_shot.py mc2_17 28 tv-hdr; py -3 scripts\quick_shot.py mc2_24 28 tv-hdr
Remove-Item Env:MC2_HDR_POST

# HDR + tonemap
$env:MC2_HDR_POST="1"; $env:MC2_TONEMAP_ACES="1"
py -3 scripts\quick_shot.py mc2_03 28 tv-tonemap; py -3 scripts\quick_shot.py mc2_17 28 tv-tonemap; py -3 scripts\quick_shot.py mc2_24 28 tv-tonemap
Remove-Item Env:MC2_HDR_POST, Env:MC2_TONEMAP_ACES

# HDR + bloom + tonemap
$env:MC2_HDR_POST="1"; $env:MC2_BLOOM="1"; $env:MC2_TONEMAP_ACES="1"
py -3 scripts\quick_shot.py mc2_03 28 tv-full-post; py -3 scripts\quick_shot.py mc2_17 28 tv-full-post; py -3 scripts\quick_shot.py mc2_24 28 tv-full-post
Remove-Item Env:MC2_HDR_POST, Env:MC2_BLOOM, Env:MC2_TONEMAP_ACES

# SSAO only (+ optional SSAO debug grayscale)
$env:MC2_SSAO="1"
py -3 scripts\quick_shot.py mc2_03 28 tv-ssao; py -3 scripts\quick_shot.py mc2_17 28 tv-ssao; py -3 scripts\quick_shot.py mc2_24 28 tv-ssao
$env:MC2_SSAO_DEBUG="1"; py -3 scripts\quick_shot.py mc2_24 28 tv-ssao-debug; Remove-Item Env:MC2_SSAO_DEBUG
Remove-Item Env:MC2_SSAO

# full stack
$env:MC2_HDR_POST="1"; $env:MC2_BLOOM="1"; $env:MC2_TONEMAP_ACES="1"; $env:MC2_SSAO="1"
py -3 scripts\quick_shot.py mc2_03 28 tv-all; py -3 scripts\quick_shot.py mc2_17 28 tv-all; py -3 scripts\quick_shot.py mc2_24 28 tv-all
Remove-Item Env:MC2_HDR_POST, Env:MC2_BLOOM, Env:MC2_TONEMAP_ACES, Env:MC2_SSAO
```

Direct-launch (drive camera; bypasses the v0.3 hardcode):

```powershell
$env:MC2_SMOKE_MODE="1"; $env:MC2_HEARTBEAT="1"; $env:MC2_HDR_POST="1"; $env:MC2_TONEMAP_ACES="1"
A:\Games\mc2-opengl\mc2-win64-v0.4\mc2.exe --profile stock --mission mc2_24 --duration 120
Remove-Item Env:MC2_HDR_POST, Env:MC2_TONEMAP_ACES
```

### Expected visual failure modes to inspect (per lane, post stack ON)

From static analysis of the post pass order (water + VFX both draw into the
RGBA16F `sceneFBO_` BEFORE the post stack, so both are tonemapped and
bloom-eligible; SSAO darkens before bloom; the sunset grade + exposure always
run):

- **Washed/flat terrain** under ACES (input trimmed x0.9, `postprocess.frag:35`).
- **Water reads dark, not bright** — base water luma is well below the 1.2
  bright-pass threshold and below the ACES knee; raise `exposure`, do not cut it,
  on water maps. (`gos_terrain_water_mdi.frag:158-168`.)
- **VFX additive blowout / hard bloom** — additive particles use
  `GL_SRC_ALPHA, GL_ONE` (`gos_particle_bridge.cpp:455`) and accumulate past 1.0
  in the HDR buffer, so dense explosions exceed 1.2 and bloom hard. Lever:
  lower `vfxAdditiveBrightness` (profile-backed) and/or raise `bloomThreshold`.
- **SSAO halos** around mechs/props (half-res GTAO-lite).
- **Shadow acne / peter-panning** (bias factor/units, ImGui-only).
- **Static-prop specular mismatch** vs terrain/mech (PBR V1 gate default-OFF;
  IBL strength 0.5).
- **Mech too dark/bright** vs terrain — no per-mission mech ambient/spec control
  yet (closed by Slice 2).

Output naming convention used above: `<mission>-tv-<config>.png`.

---

## 5. Candidate problems to inspect (drives Slice 4 after captures)

These are hypotheses to confirm/deny with the captures, not confirmed defects:

1. Mech vs terrain ambient mismatch on mixed maps (mc2_17) — mech ambient is a
   flat 0.15 hemisphere; terrain V1 may be gated off. Inspect whether mechs read
   flatter than the ground they stand on.
2. VFX additive blowout under bloom on mech-heavy combat (mc2_24).
3. Water darkening under ACES on terrain/water maps (mc2_03).
4. Static-prop specular absence (PBR V1 default-OFF) leaving buildings reading
   matte next to specular mechs.
5. Exposure: only mc2_24 has a non-default (1.1). Determine per-mission exposure
   needs once HDR is the intended default.

---

## 6. Work classification

| Class | Item |
|-------|------|
| Must-do (consistency) | Slice 1 (this doc) + Slice 3 (capture matrix above) |
| Should-do (code, review-safe) | Slice 2: mech ambient/specular profile keys + writer-asymmetry fix |
| Nice-to-have, deferred | water-reflection-strength key (gate-coupled), static-prop PBR-strength key, shadow-bias keys, terrain normals/tint keys, Slice 5 terrain debug-registry promotion |
| Defer until captures | Slice 4 candidate per-mission profile values (needs eyeball) |
| Do not touch | sun color/dir (map-driven), screen-shadow `0.9` literal, renderer architecture, asset pipeline, shadow/water algorithms, HZB/culling |

---

## 7. Status log

- Recon (5 subagents) + this consolidated reference (Slice 1 + 3): COMPLETE
  (commit `f15bc706`).
- Slice 2 (mech ambient/specular profile keys + reader/writer asymmetry fix):
  SHIPPED (commit `93844db8`). `mechAmbientStrength` / `mechSpecularStrength`
  added to `applyKey` (env-guarded on `MC2_MECH_AMBIENT_V1_STRENGTH` /
  `MC2_MECH_SPECULAR_STRENGTH`); `saveCurrentToMission` now round-trips the full
  reader vocabulary (bloom/AO drop-bug fixed + the two mech keys). `visual_tuning.json`
  untouched -> keys dormant by default (missing-key = engine default).
  VALIDATION: check-contracts 8/8; clean full build (exe links, key strings
  present in exe); runtime launch with a temp profile
  (`MC2_VISUAL_TUNING_FILE`) on mc2_24 logged `[VisualTuning] mission='mc2_24'
  applied 6 keys` with NO unknown-key warnings, 2769 frames to clean shutdown,
  zero GL errors. Deployed exe+pdb+DLLs+83 shaders to v0.4 (all diff-verified;
  v0.4 had a non-nifty shader set, normalized to match the exe).
- FOLLOW-UP (explicit user request, APPROVED default change): mech specular
  strength default **1.0 -> 0.05** (1.0 was blown out). Applied in BOTH places:
  engine default `s_mechSpecularStrength` (`gos_mech_batcher.cpp:202`) AND the
  `data/visual_tuning.json` `defaults` block (`mechSpecularStrength: 0.05`). This
  is the one intentional default visual change in this branch -- it edits the
  shipped JSON and changes shipped behavior, by explicit instruction. Validated:
  rebuilt clean; default-profile mc2_24 launch logged `applied 10 keys` (incl.
  mechSpecularStrength), no unknown-key, no GL errors, 600 frames clean shutdown.
- Slices 4 (candidate per-mission values) / 5 (terrain debug-registry promotion):
  deferred per classification above. Slice 4 needs the human eyeball pass on the
  Section 4 captures first; the new mech keys are the lever it will use.
