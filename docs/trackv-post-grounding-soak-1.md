<!-- doc-status: current | reviewed:2026-10-11 -->
# VISUAL-CAPTURE-MATRIX-SOAK-1: Track V Post + Grounding MVP

**Branch:** `claude/trackv-post-grounding-mvp`
**Date:** 2026-05-29
**Purpose:** First reusable capture/soak matrix for the Track V HDR-post + SSAO
grounding MVP stack. Validate that the default path is unchanged and that each
gate-ON combination is safe + tunable.
**Status:** READY — commands below; captures run by the user (gate-ON visuals
need a human eye + a driven camera).

---

## What exists at soak time

| Slice | Gate (env) | Default | What it adds |
|---|---|---|---|
| TRACKV-GATE-DEFAULT-OFF-TEST-1 | — | — | RenderCore unit test: all Track V gates Feature-kind, default-OFF |
| HDR-POST-SCAFFOLD-1 | `MC2_HDR_POST` | OFF | Master gate for the HDR post stack (scene FBO is already RGBA16F) |
| BLOOM-MVP-1 | `MC2_BLOOM` | OFF | Threshold + half-res ping-pong bloom (existing `runBloom`); needs `MC2_HDR_POST` |
| TONEMAP-ACES-MVP-1 | `MC2_TONEMAP_ACES` | OFF | ACES filmic curve (existing `postprocess.frag`); needs `MC2_HDR_POST` |
| SSAO-GTAO-LITE-MVP-1 | `MC2_SSAO` | OFF | Half-res world-space AO grounding pass (`runSSAO`); independent |

**Dependencies:** `MC2_BLOOM` and `MC2_TONEMAP_ACES` are inert without
`MC2_HDR_POST=1` (the master gate force-disables them in the composite).
`MC2_SSAO` is independent of the HDR master gate.

**Default behavior declaration:** with NONE of these env vars set, output is
byte-identical to pre-Track-V (`runBloom`/`runSSAO` early-return; composite
forces `enableBloom`/`enableTonemap` to 0; exposure stays 1.0). The
pre-existing unconditional sunset grade in `postprocess.frag` is untouched.

---

## Env var + tunable reference

| Env var | Effect | Default |
|---|---|---|
| `MC2_HDR_POST=1` | enable HDR post stack (master) | OFF |
| `MC2_BLOOM=1` | enable bloom (requires master) | OFF |
| `MC2_TONEMAP_ACES=1` | enable ACES tonemap (requires master) | OFF |
| `MC2_SSAO=1` | enable SSAO grounding pass | OFF |
| `MC2_SSAO_DEBUG=1` | SSAO apply overwrites scene with AO grayscale | OFF |

Per-mission tunables (in `data/visual_tuning.json`, `defaults` or per-mission
block — missing keys keep engine defaults):

| Key | Range | Default | Slice |
|---|---|---|---|
| `exposure` | ≥0 | 1.0 | tonemap/HDR |
| `bloomThreshold` | 0..4 | 1.2 | bloom (tuned 2026-05-29, was 0.6) |
| `bloomIntensity` | 0..4 | 0.15 | bloom (tuned 2026-05-29, was 0.3) |
| `aoRadius` | 0.1..64 (world units) | 3.0 | ssao |
| `aoStrength` | 0..2 | 0.7 | ssao |
| `aoBias` | 0..0.1 | 0.0025 | ssao |

These are also live-adjustable via Graphics Options → **Post-Process → Track V**
ImGui sliders (`Ctrl+Shift+G`) when `MC2_IMGUI=1`.

**Tuned defaults (2026-05-29, first soak):** bloom intensity halved (0.15) and
threshold doubled (1.2) — stock bloom was a touch strong; the ACES tonemap
input is trimmed ~10% (`* 0.9` in the tonemap branch of `postprocess.frag`) so
the filmic result reads slightly less hot. All three are tonemap/bloom-branch
only, so the default-OFF path stays byte-identical.

**Caveat — HDR-master ImGui toggle:** the shipped default is gated OFF via the
`MC2_HDR_POST` **env var**, and that path is smoke-verified to render normally
(not black). Un-checking the "HDR Post (master)" checkbox in ImGui *after* the
stack has been running has been observed to darken the view; prefer toggling
the feature via the env var / a fresh launch. The env default-OFF path (what
ships) is unaffected. (Follow-up: confirm/repro the runtime-toggle asymmetry.)

---

## How to launch a single mission (NO --kill-existing)

Do **not** run `run_smoke.py --kill-existing` and do **not** run a soak
concurrent with another mc2.exe trace. Deploy fresh exe + shaders first
(shaders MUST include `ssao.frag` + `ssao_apply.frag` or SSAO programs fail to
compile). Then either:

```powershell
# Capture: launches mc2.exe, waits N seconds, screenshots to
# tests/smoke/artifacts/diag-shots/<mission>-<label>.png
py -3 scripts\quick_shot.py mc2_24 28 trackv-default
```

```powershell
# Direct visual inspection (drive the camera yourself):
$env:MC2_SMOKE_MODE="1"; $env:MC2_HEARTBEAT="1"
A:\Games\mc2-opengl\mc2-win64-v0.4\mc2.exe --profile stock --mission mc2_24 --duration 120
```

Set Track V env vars in the SAME shell before launch (they are resolved once at
init):

```powershell
$env:MC2_HDR_POST="1"; $env:MC2_TONEMAP_ACES="1"
A:\Games\mc2-opengl\mc2-win64-v0.4\mc2.exe --profile stock --mission mc2_24 --duration 120
Remove-Item Env:MC2_HDR_POST, Env:MC2_TONEMAP_ACES   # clear after
```

Suggested missions: `mc2_03` (terrain/sky-heavy), `mc2_17` (mixed),
`mc2_24` (mech-heavy + buildings/water).

---

## Capture matrix

Screenshots land in `tests/smoke/artifacts/diag-shots/`. Run each row across
the three suggested missions.

### Row 1 — Default (all gates OFF) — REGRESSION BASELINE

```powershell
py -3 scripts\quick_shot.py mc2_03 28 trackv-default
py -3 scripts\quick_shot.py mc2_17 28 trackv-default
py -3 scripts\quick_shot.py mc2_24 28 trackv-default
```

**Expect:** identical to current shipped look. This is the byte-identical
control. Any difference here is a bug — stop.

### Row 2 — HDR only (`MC2_HDR_POST=1`)

```powershell
$env:MC2_HDR_POST="1"
py -3 scripts\quick_shot.py mc2_24 28 trackv-hdr
Remove-Item Env:MC2_HDR_POST
```

**Expect:** still ~identical (master gate alone enables nothing visible until a
sub-feature or a tuned exposure is applied). Confirms the scaffold is a no-op
on its own.

### Row 3 — HDR + Tonemap (`MC2_HDR_POST=1 MC2_TONEMAP_ACES=1`)

```powershell
$env:MC2_HDR_POST="1"; $env:MC2_TONEMAP_ACES="1"
py -3 scripts\quick_shot.py mc2_03 28 trackv-tonemap
py -3 scripts\quick_shot.py mc2_24 28 trackv-tonemap
Remove-Item Env:MC2_HDR_POST, Env:MC2_TONEMAP_ACES
```

**Inspect:** bright sky / water highlights / mech specular — should be gently
compressed (filmic), not washed or crushed. UI/HUD must stay readable and NOT
look double-tonemapped (UI composites after `endScene`).

### Row 4 — HDR + Bloom + Tonemap (full post stack)

```powershell
$env:MC2_HDR_POST="1"; $env:MC2_BLOOM="1"; $env:MC2_TONEMAP_ACES="1"
py -3 scripts\quick_shot.py mc2_03 28 trackv-full-post
py -3 scripts\quick_shot.py mc2_24 28 trackv-full-post
Remove-Item Env:MC2_HDR_POST, Env:MC2_BLOOM, Env:MC2_TONEMAP_ACES
```

**Inspect:** bloom only on genuinely bright pixels (no full-screen haze, no
washed terrain). UI must have NO bloom (composited after). If terrain washes
out, lower `bloomIntensity` / raise `bloomThreshold` in the profile.

### Row 5 — SSAO only (`MC2_SSAO=1`, independent)

```powershell
$env:MC2_SSAO="1"
py -3 scripts\quick_shot.py mc2_24 28 trackv-ssao        # mech + buildings
py -3 scripts\quick_shot.py mc2_03 28 trackv-ssao        # terrain
Remove-Item Env:MC2_SSAO
```

**Inspect:** contact darkening under mechs / at building bases / terrain
crevices (grounding). Check for halos at terrain↔sky silhouettes and shimmer.
Sky + UI must be untouched (AO=1 on sky; UI composites after).

### Row 6 — SSAO debug view (`MC2_SSAO=1 MC2_SSAO_DEBUG=1`)

```powershell
$env:MC2_SSAO="1"; $env:MC2_SSAO_DEBUG="1"
py -3 scripts\quick_shot.py mc2_24 28 trackv-ssao-debug
Remove-Item Env:MC2_SSAO, Env:MC2_SSAO_DEBUG
```

**Inspect:** the raw half-res AO buffer as grayscale — occlusion should hug
geometry contacts; large flat areas should read near-white (unoccluded).

### Row 7 — Full MVP stack (everything ON)

```powershell
$env:MC2_HDR_POST="1"; $env:MC2_BLOOM="1"; $env:MC2_TONEMAP_ACES="1"; $env:MC2_SSAO="1"
py -3 scripts\quick_shot.py mc2_24 28 trackv-mvp-all
Remove-Item Env:MC2_HDR_POST, Env:MC2_BLOOM, Env:MC2_TONEMAP_ACES, Env:MC2_SSAO
```

**Inspect:** combined feel — more "modern" without being washed/dark; mech +
building readability preserved; water/sky sane; no GL errors in `game.log`.

---

## What to record per capture

- Visual quality (subjective): more modern? natural?
- Washed-out / overly-dark areas
- UI readability (HUD bars, brackets, text)
- Mech readability (silhouette, team color)
- Building / terrain grounding (does SSAO seat objects?)
- Water + sky behavior (no AO bleed, no double-tonemap)
- GL errors: grep `game.log` for `GL error` / `0x050`
- Rough performance: Tracy `Render.SSAO` / `Render.PostProcess` zones if captured

---

## Expected failure modes

| Symptom | Likely cause | Action |
|---|---|---|
| Row 1 differs from current | a gate not truly default-OFF | STOP — regression |
| SSAO buffer all black/white | depth/normal/matrix convention wrong | check `MC2_SSAO_DEBUG`, tune radius/bias |
| SSAO halos at sky edge | range check too wide | lower `aoRadius` |
| Washed terrain with bloom | intensity too high / threshold too low | tune profile |
| UI looks dark/washed | composite order regression | STOP — fix composite |
| Black terrain on gate-ON | shaders not deployed (`ssao*.frag`) | redeploy shaders |

---

## Per-mission tuning notes

Start global defaults conservative (the hardcoded member values). Tune
per-mission in `data/visual_tuning.json` once captures identify maps that need
it — e.g. a bright desert map may want a lower `exposure` under tonemap; a
dense urban map may want a slightly larger `aoRadius`. Missing keys / missing
file preserve current behavior (backward-compatible).
