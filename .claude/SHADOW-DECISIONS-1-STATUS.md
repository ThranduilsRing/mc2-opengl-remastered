# SHADOW-DECISIONS-1 — STATUS (2026-07-26)

Lane SHADOW-DECISIONS-1. Worktree `A:/Games/mc2-main`, branch
`claude/nifty-mendeleev`.

- Commits landed: `6a02565e`, `092fab0e` (both mine).
- Final verified HEAD: **`74fec4957198`** (includes sibling DETERMINISM-RATCHET-2
  commits `5efbb2e6`/`9cd10896`/`74fec495` that landed mid-session).
- Deploy: `A:/Games/mc2-opengl/mc2-win64-v0.3`, `deploy_status` verdict **OK**,
  manifest `src_commit 74fec4957198`, no dirty flag.
- **tier1: PASS (5/5) with explicit `DEPLOY_FINGERPRINT OK` on all five missions**
  (`exe sha=74fec4957198 matches worktree HEAD`). Contracts **70/70**.
- Evidence: `scratch/shadowdec1/`. All PNGs verified non-zero (1.44 MB each).
  Disk held at ~13 GB free throughout.

| Objective | Verdict |
|---|---|
| A — `MC2_SSAO` default | ❌ **DO NOT DEFAULT ON.** Measured benefit is ZERO at shipped tuning (§3). |
| A — `MC2_PROP_SHADOW_RECEIVE` default | ❌ **DO NOT DEFAULT ON.** Real but rare; cannot touch the user's complaint (§2, §4). |
| A — user's building-face complaint | ⚠ **NEITHER GATE CAN FIX IT.** Root cause identified and it is a third thing (§2). |
| B — LOD ≥ 1 shadow casting | ✅ **Gate landed, default-OFF, proven live.** Benefit **undemonstrated** (§5). |
| B — do blob shadows return? | ⚠ **No blob observed — but no new far-LOD shadow was observed either** (§5). |
| Palm-vs-pine same-frame capture | ❌ **NOT DONE** (§7). |
| Tracy perf matrix | ❌ **NOT DONE — Tracy is not compiled in any build tree** (§6). |

---

## 1. META REVIEW — inherited premises, checked against source

| # | Inherited claim | Reality |
|---|---|---|
| C1 | `bdactor.cpp:7436` "excludes every LOD ≥ 1 from casting shadows" | **Materially narrower.** The `setRecipeNoShadow` flag is consulted in exactly ONE place: `getDynamicPropShadowInstances` (`gos_static_prop_registry.cpp:1895`) — the full-registry replay. `flushShadow`'s camera-visible loop (`gos_static_prop_batcher.cpp:8698-8710`) has **no** noShadow/LOD filter, so **visible LOD1 types already cast today.** Only the off-screen/replay set was suppressed. This predicts a small on-screen effect — and §5 measures exactly that. |
| C2 | The "no alpha discard" justification is false since `b8f764b6` | ✅ **CONFIRMED.** `resolvePropShadowProgram` (`:8461`) binds `shadow_static_prop_alpha_atlas` → `shadow_static_prop_alpha`, both default-ON (`:8437`, `:8449`), both running `tex_color.a < 0.5 → discard` (`shaders/shadow_static_prop.frag`). The empty `shadow_instanced.frag` that motivated the exclusion is not in that ladder at all. |
| C3 | Blob risk: do lod≥1 props carry ALPHA_TEST? | ✅ **They do**, identically to LOD0 — every imported LOD gets `SetAlphaTest(true)`. Higher LODs are real lower-poly ASE imports, not billboards. Per-packet atlas sidecars are indexed by GLOBAL packet index in `finalizeGeometry()`, so far-LOD packets bind valid layers; a missing layer fails **safe** (`bindPropShadowAtlasPacket` clears ALPHA_TEST → opaque depth), never a wrong-layer sample. |
| C4 | "Tree is clean, contracts 69/69" | Tracked tree clean (`--porcelain -uno`); `preflight` reports `safe_to_touch=false` purely from untracked `scratch/`+`debug_state/`. Contracts are **70/70** now, not 69 — the sibling added a checker. |
| C5 | Deploy to `0.4c` | **Rejected.** A foreign process (PID 37956) held the `0.4c` exe. Did not kill it. Used my own `0.3` lane. |
| C6 | HEAD is stable during the lane | **False twice.** HEAD moved `092fab0e → 74fec495` mid-session (DETERMINISM-RATCHET-2). Caught by the first tier1's `DEPLOY_FINGERPRINT MISMATCH` — which printed **alongside `PASS (5/5)`**, the exact trap the brief named. Rebuilt + redeployed and re-ran to get a genuine OK. |

**Environment hazard found:** the shell profile injects
`MC2_GPU_CULL_STATIC_EAGER_LIGHT_BAKE`, `MC2_GPU_CULL_STATIC_FROZEN_RECORDS`,
`MC2_STATIC_POP_SPLIT`. `run_smoke.py` now hard-refuses on these (sibling's
`9cd10896`). They must be unset **in every shell invocation** (agent bash state
does not persist). They WERE present in all `run_visual_capture` arms — but
identically in every arm, so all A/Bs remain valid ON-vs-ON controls.

---

## 2. ★ Why `MC2_PROP_SHADOW_RECEIVE` cannot fix the user's complaint

The brief asked me to understand *why* the effect is so small before deciding,
and hypothesised cascade resolution. **The real reason is more fundamental and
it is architectural, not a tuning problem.**

Static-prop lighting is (`shaders/static_prop.vert:781`, the debug decomposition
is the clearest statement of the sum):

```
lit = base_light  +  sunColor * ndl  +  ambient/fill
```

- `base_light` is `get_base_light(a_aRGBLight)` — a **baked per-vertex value from
  the asset**. It is entirely orientation-independent.
- `shadow_screen.frag` computes a **sun-shadow term only** ("this pass now applies
  sun (static + dynamic) shadow only", `:323`). `MC2_PROP_SHADOW_RECEIVE` adds one
  `sampleDynamicShadow(..., 0.4)` `min()`-combined into that term (`:337-355`).

The user's complaint is *"this building face should be in shadow as the light is
from behind"*. A face with the light behind it **already has `ndl = 0`**, so its
sun contribution is already zero. Attenuating a zero term by a shadow factor
changes nothing. The face is bright because of `base_light`, which no shadow term
touches.

⇒ `MC2_PROP_SHADOW_RECEIVE` is **architecturally incapable** of darkening a
back-face. It can only help a prop pixel that IS facing the sun but is occluded
by another prop (prop-on-prop). Those are genuinely rare — which is precisely why
the prior lane measured 0.003–0.10 %.

This is a *better* explanation than the cascade-resolution hypothesis, because it
also predicts the shape of the measurement I actually got in §4: **not weak
everywhere, but strong-where-it-acts and acting almost nowhere.**

**Where the real fix lives:** `base_light` is a per-vertex constant with no
orientation term. Making prop base light hemispherical/directional (or letting
`ndl` modulate it) is the change that would darken back-faces. That is a
different, larger slice and is NOT attempted here.

---

## 3. ★ `MC2_SSAO` — measured benefit is ZERO at shipped defaults

`MC2_SSAO_DEBUG=1` overwrites the frame with the raw AO buffer. On mc2_01
buildings (3 poses), the AO buffer is **essentially pure white**:

| pose | AO min | AO mean | frac < 250 | frac < 200 |
|---|--:|--:|--:|--:|
| airport_bldgs_close | 251 | 254.992 | 0.000 % | 0.0000 % |
| airport_bldgs_low   | 252 | 254.996 | 0.000 % | 0.0000 % |
| airport_bldgs_mid   | 250 | 254.989 | 0.000 % | 0.0000 % |

**Maximum occlusion anywhere in any frame is ~2 % darkening. Not one pixel falls
below 250/255.** The corresponding scene delta confirms it: SSAO ON vs OFF
changes 0.36–0.62 % of pixels at **maxAbs 2–4 out of 255** — below any visible
threshold. At **night (mc2_10, 3 poses) SSAO ON is byte-identical to OFF.**

Cause: `aoRadius_` defaults to **3.0 world units** (`gos_postprocess.cpp:758`),
which is far too small for MC2's world scale, so the hemisphere samples never
reach a neighbouring surface. `aoStrength_` is 0.7/2.0 — not the limiting factor.
There is **no `MC2_SSAO_RADIUS`/`_STRENGTH` env override**; tuning is ImGui /
per-mission profile only, so 3.0 is what ships.

**Recommendation: leave `MC2_SSAO` default-OFF.** This holds regardless of its
cost — a full-screen half-res pass plus composite every frame, for a maximum 2 %
darkening and a byte-identical night, is a pure loss. **Separately: SSAO should
be treated as mistuned/latent-broken, not merely "off".** The honest follow-up is
a radius sweep, not a default flip.

---

## 4. `MC2_PROP_SHADOW_RECEIVE` — measured

Noise floor first (the brief's requirement). Two identical-config mc2_01 runs
(`wbase` vs `wbase2`, 9 poses): **8/9 poses byte-identical; 1 pose
(cross_map_view) 0.001 %, maxAbs 115** — mover sprites. mc2_10 night baseline
repeated twice: **all 3 poses hash-identical, 0.000 %.**

| map / pose set | changed % | maxAbs |
|---|--:|--:|
| mc2_01 buildings close | 0.005 % | 136 |
| mc2_01 buildings low | 0.005 % | 138 |
| mc2_01 buildings mid | 0.183 % | 144 |
| mc2_01 wide (9 poses) | ≤ 0.051 % | ≤ 255 |
| **mc2_10 night (3 poses)** | **0.000 %** | **0** |

The effect is **real and strong where it lands, but lands almost nowhere** —
exactly as §2 predicts. Visual inspection of the largest day case
(`airport_bldgs_mid`, `scratch/shadowdec1/recv_mid_crop.png`): the changed pixels
are concentrated on **one small aircraft prop**. **The building faces are
unchanged ON vs OFF.** The user's complaint reproduces identically in both states.

**Recommendation: leave `MC2_PROP_SHADOW_RECEIVE` default-OFF.** Not because it is
wrong — the fence class from `d99bd798` works and it is correctly scoped — but
because it moves ≤0.05 % of pixels on a typical frame, nothing at night, and
**does not address the complaint it was nominated to fix.** Flipping it would
spend a shipped-pixel change and 9 golden drifts on an invisible improvement.

---

## 5. Task B — `MC2_PROP_LOD_SHADOW_CAST` landed; benefit undemonstrated

**Landed** (`6a02565e` + enum fix `092fab0e`): the `setRecipeNoShadow(regIdx,
true)` call in `TreeAppearance::registerStatic` is now wrapped in
`if (!s_propLodShadowCast)`. Gate is **registered** (`RendererFeature::
PropLodShadowCast = 50`, COUNT 51), **documented** (`docs/tier1_env_vars.md`),
**smoke-allowlisted** (`run_smoke.py`), `gate_status` verdict **OK**, and the
string is present in the deployed exe (binary grep = 1 hit). Default OFF →
caster set byte-identical.

**Proven live, not env-dropped:** on mc2_03 scout, poses 0 and 1 change hash with
the gate ON (`a52f469b→cc610055`, `54aaefa3→bb9567a0`). The zeros elsewhere are
genuine "no far-LOD props contributing here", not a dropped env var.

**But the benefit is undemonstrated.** Across 14 day poses + 3 night poses:

| pose set | changed % | vs noise floor |
|---|--:|---|
| mc2_01 buildings (3) | 0.000 % | at floor |
| mc2_01 wide (9) | ≤ 0.013 % | straddles floor |
| mc2_03 scout (9) | ≤ 0.029 % (2 poses) | above floor |
| mc2_24 terrain_near (2) | 0.000 % | at floor |
| mc2_10 night (3) | 0.000 % | at floor |

I cropped and **looked at** the two largest cases. `mc2_01 runway_overview`
(0.013 %): only 13 pixels above threshold, and they are **mover sprites with
health bars, not tree shadows**. `mc2_03 scout_0` (0.026 %): 30 pixels at high
altitude, no identifiable new shadow silhouette.

**This is consistent with the C1 scope correction, and I regard C1 as the
explanation:** visible far-LOD props already cast via `flushShadow`, so
un-suppressing the *replay* set adds shadows mostly from props that are
off-screen, whose shadows largely do not land in view either.

**Blob question, answered honestly:** no rectangular blob artifact was observed
in any capture — **but no new far-LOD foliage shadow was observed either**, so
this is *not* a positive proof that blobs cannot return. The strong static
argument that they cannot is in §1/C2–C3 (both bound programs alpha-discard;
lod≥1 carries ALPHA_TEST; degenerate case fails safe to opaque depth). A
positive visual proof needs a pose with known far-LOD foliage in frame, which I
did not find in the existing bookmark set.

**Recommendation:** keep `MC2_PROP_LOD_SHADOW_CAST` **default-OFF**. The code
correction is worth having landed (the stale comment was actively misleading and
would have blocked the next investigator), but there is no measured benefit to
justify growing the distance caster set. Reopen only with a bookmark that puts
far-LOD trees in frame.

---

## 6. Perf — NOT MEASURED, and why

**Tracy is not compiled into any build tree.** `build64/CMakeCache.txt` has zero
Tracy entries, and so do all 28 sibling `build64-*` dirs. `tools/tracy_capture_
compare.py matrix` (the `PERF-HARNESS-TRUTH-1` path the brief mandates) requires
Tracy zones; the Tracy CLI tools exist (`tracy-csvexport.exe`) but there is
nothing to capture.

Measuring properly would mean configuring and fully building a second Tracy
tree. With **A: at 99 % (13 GB free) and a documented zero-disk incident that
silently produced 12 zero-byte PNGs**, I judged spending several GB on a second
build tree to be the wrong risk — **especially because the perf number cannot
change either recommendation**: SSAO's measured benefit is zero, so its cost is
irrelevant to the decision, and RECEIVE/LODCAST are both staying OFF.

I am recording this as **not done** rather than substituting an argued bound.
If a perf number is wanted for its own sake, the prerequisite is a Tracy-enabled
build dir and ~1 GB of disk.

---

## 7. Not done / not claimed

- **Palm-vs-pine same-frame low-sun capture — NOT DONE.** No bookmark in
  `tests/visual/bookmarks/` targets palms or a low-sun foliage pose, and hunting
  one by hand was out of budget after the mid-session HEAD move forced a full
  rebuild/redeploy/re-tier1 cycle. The "asset geometry, not a shader bug" ruling
  therefore remains **source-only and unconfirmed** — unchanged by this lane.
- **Full 4-map × day/night × ≥3-location matrix — PARTIAL.** Covered: mc2_01 day
  (12 poses across 2 sets), mc2_03 day (9), mc2_24 day (2), mc2_10 night (3).
  **Missing: night on mc2_01/mc2_03/mc2_24.** Justification for stopping: the
  night arm I did run returned byte-identical output for *all three* gates, and
  §2/§3 explain mechanistically why night is a null case (RECEIVE modulates a sun
  term; SSAO's AO buffer is white). Extending the night matrix would very likely
  add more zeros.
- **Fence casting-onto-ground evidence — NOT re-verified.** The prior lane's
  "fences do not self-shadow as slabs" result stands unrefuted and I did not
  contradict it, but I did not independently reproduce the fence zoom, and I did
  **not** resolve its open sub-question (no fence shadow visible on the ground in
  either state).
- **Golden regions** — not graded; no golden re-bless proposed. No release
  artifacts touched (`release_assets/`, `releases/` are user-gated).
- No push, no `--amend`, no foreign-process kills.

---

## 8. Recommended successor sequence

1. **Do not flip any of the three gates.** All three should stay default-OFF on
   this evidence.
2. **The user's building-face complaint is still open and now has a named root
   cause** (§2): `base_light` is a per-vertex baked constant with no orientation
   term. The candidate slice is a hemispherical/directional base-light term for
   static props — measure on the mc2_01 buildings poses, which show the defect.
3. **SSAO needs a radius sweep, not a default decision.** `aoRadius_ = 3.0` is
   almost certainly wrong for MC2's world scale. Add `MC2_SSAO_RADIUS` /
   `MC2_SSAO_STRENGTH` env overrides (there are none today) and re-run the
   `MC2_SSAO_DEBUG` AO-buffer capture in §3 — that one capture is a 30-second
   decisive test for any candidate radius.
4. **Task B**: author a bookmark with far-LOD foliage in frame before revisiting
   `MC2_PROP_LOD_SHADOW_CAST`. Without one, neither the benefit nor the blob
   question can be settled visually.
5. **Unset the three profile-injected `MC2_*` vars in every shell**, or
   `run_smoke.py` will refuse to run.
