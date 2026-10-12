<!-- doc-status: current | reviewed:2026-10-11 -->
# MC2 Visual Regression Lab Architecture

**Status:** Strategy / design doc (no code yet), 2026-06-11.
**Scope:** Make visual verification systematic — replace "launch game and eyeball it" with deterministic camera bookmarks, golden frame captures, numeric diff verdicts, and a human-reviewable artifact browser.
**Siblings:** `telemetry-oracle-cockpit-architecture.md` (the lab's reports are cockpit artifacts — same run-folder species, same severity model, same HTML skin), `runtime-bridge-architecture.md` (process supervision), `docs/VISUAL-CAPTURE-MATRIX-1-DESIGN.md` (the existing capture-tuple design this doc subsumes and extends), `docs/mcp-render-state.md` (agent access path).

---

## 1. North star

> **A visual regression is detected by a machine, localized by a number, and confirmed by a human looking at exactly one pre-rendered diff image — never by anyone "flying around to see if it looks right." Agents get numeric verdicts; humans get curated diff galleries; the engine stays the only producer of pixels.**

Load-bearing consequences:

1. **Agents are blind — design for it.** Every lab conclusion must be expressible as `exit code + JSON numbers` consumable headlessly (the same contract as `telemetry_compare.py` exit 0/1/2 in the cockpit doc §6). Diff *images* are saved for the human, never interpreted by the agent. An agent's job ends at "bookmark `mc2_01/ridge_water` FAIL: 2.31% changed pixels outside mask, diff at <path> — needs human eyes."
2. **The engine captures its own pixels.** OS-level grabs (`pyautogui.screenshot` in `scripts/camera_sweep_smoke.py`; the capture_baseline.py one was retired 2026-10-12, cull wave 1) are compositor-poisoned (1–3 LSB drift, DWM scaling — documented in `docs/VISUAL-CAPTURE-MATRIX-1-DESIGN.md` Known Limitations). The lab's capture primitive is in-engine `glReadPixels`/PBO readback at a settled frame, written as PNG by mc2.exe itself. The "glReadPixels deferred" slice from VISUAL-CAPTURE-MATRIX-1 is this lab's Slice 1.
3. **The capture tuple is sacred.** A capture is the deterministic function `(build, mission, bookmark, preset, gate-env, settle-rules, seed) → pixels + state JSON`. Same tuple, same build → byte-comparable pixels (modulo the declared nondeterminism masks, §5). The seed already exists (`MC2_SMOKE_SEED`, `GameOS/gameos/gos_smoke.cpp:50`).
4. **Visual verdicts are advisory until promoted.** Per the cockpit doc §8 severity model: the lab produces WARN, never FAIL, until a bookmark+metric pair has proven stable across tier1 5/5 over multiple builds. `smoke_lib/gates.py` remains the sole hard-gate authority; visual gates enter it only by the cockpit promotion rule.
5. **Cheap checks before expensive ones.** Oracle counters (`[RENDER_SNAPSHOT v3]`, `render_snapshot.h:195-204` ok aggregator) and draw/state counts run *before* any pixel diff (§6). If the oracle says the frame's composition changed, the pixel diff is confirmation, not discovery.

---

## 2. Capture model — bookmarks, fixtures, settle rules

### 2.1 Camera bookmarks

A **bookmark** is a named deterministic camera pose bound to a mission fixture:

```json
// tests/visual/bookmarks/mc2_01.json
{
  "schema_v": 1,
  "mission": "mc2_01",
  "bookmarks": [
    { "name": "overview_spawn",  "pos": [x,y,z], "rot": deg, "proj": deg, "zoom": f,
      "covers": ["terrain_splat", "water_shoreline", "sky"] },
    { "name": "ridge_shadow",    "pos": [..], "...": "...",
      "covers": ["shadow_cascade", "terrain_lod_chunk_skirts"] },
    { "name": "props_closeup",   "covers": ["static_prop_pbr", "tree_alpha"] }
  ]
}
```

- **Stored** as JSON in git under `tests/visual/bookmarks/<mission>.json` — reviewable, diffable, blameable. NOT in save files or pak data.
- **Authored** two ways: (a) in-game hotkey (`MC2_VISUAL_BOOKMARK_CAPTURE=1` + key) dumps the current `Camera` pose (`code/gamecam.cpp` frame loop owns it) to stdout as a `[BOOKMARK v1]` line the author pastes into the JSON; (b) editor "Save Camera Bookmark" button later (Phase 4).
- **Replayed** by a smoke-mode extension: `MC2_VISUAL_CAPTURE=<path-to-bookmarks.json>` in `gos_smoke.cpp`'s existing smoke harness. The engine teleports the camera to each bookmark (hard set, no interpolation — `setPosition`/rotation/projection directly, bypassing input smoothing), waits the settle window, captures, advances to the next bookmark. This rides the proven `MC2_SMOKE_MODE` infrastructure rather than a new driver.
- **Saving bookmarks from in-game** (BOOKMARK-SAVE-BUTTON-1, shipped): the ImGui inspector's "Save Camera Bookmark" button (`GuiRuntime/EditorInspector.cpp` → `mc2_save_camera_bookmark()` in `code/gamecam.cpp`) writes a complete schema-valid bookmark JSON (`{"mission": "<stem>", "bookmarks": [{name,pos,rot,proj,alt}]}`, two identical entries because single-pose files silently fail). Output dir: `MC2_BOOKMARK_SAVE_DIR` if set (point it at `A:/Games/mc2-main/tests/visual/bookmarks` or your lane worktree equivalent to make the file git-trackable), else `saved_bookmarks/` under the working dir. The path is printed as `[BOOKMARK_SAVE v1] wrote ...` to stderr and shown in the panel; the file is directly consumable by `MC2_VISUAL_BOOKMARK_CAPTURE`. Workflow: see issue in game → click button → agent drives to it with the capture sweep / RenderDoc. Test hook `MC2_BOOKMARK_SAVE_TEST=1` fires one save ~120 frames after mission load.
- **`covers` tags** map bookmarks to render subsystems so a shader change can run only the bookmarks that exercise it (e.g. a `shaders/gos_terrain.frag` change runs `covers ∩ {terrain_*}` first, full set before merge).

### 2.2 Mission fixtures

Reuse the canonical tier1 set — it was chosen for biome/asset coverage already (`docs/VISUAL-CAPTURE-MATRIX-1-DESIGN.md`): `mc2_01` (grass/water), `mc2_03` (rock/dry bed), `mc2_10` (urban props/FX), `mc2_17` (splat transitions), `mc2_24` (steep terrain/shadow stress/fog). Plus `1kbasicmap` as the oversized-map fixture once its MOVE data settles.

**Idle teleport, not fly-through.** The smoke fly-through is great for crash coverage but terrible for pixel determinism (camera position is time-coupled to frame rate). The lab uses *discrete teleported poses with settle windows* — fly-throughs remain the smoke gate's job. A scripted camera *path* mode (fixed-timestep spline for video diffing) is an anti-goal for v1 (§9).

Target budget: **3–5 bookmarks per mission, ~25 bookmarks total**. Small enough to review every diff by hand; large enough to cover terrain/shadow/water/props/FX/UI-free framings.

### 2.3 Settle rules

Between teleport and capture:

1. **Frame settle:** render ≥ N frames (default 30) at the bookmark before capture — flushes texture streaming, terrain chunk LOD admission (the chunk dispatch uses frame N−1 MVP — a known 1-frame latency, MEMORY 8z handoff), shadow-map warmup (`renderStaticTerrainShadowFullMap` frame-1 warmup), and bloom history.
2. **Sim freeze:** capture with game simulation paused (`MC2_VISUAL_FREEZE_SIM=1`) — no mover animation, no AI, no projectiles. The renderer keeps presenting; only the scene graph stops mutating.
3. **Time pin:** global animation clock pinned to a fixed value per bookmark (water scroll phase, FX time, light flicker all derive from it). This is the *settling* half of nondeterminism handling; masks (§5) are the fallback for what can't be pinned.
4. **Fixed resolution + preset:** capture at **1600×900 windowed** (fits all dev monitors, divisible by 8 for BC analysis), one canonical quality preset, AA fixed (no temporal jitter — see §5).
5. **Seed:** `MC2_SMOKE_SEED` fixed per fixture.

---

## 3. Golden frame storage + versioning

### 3.1 What gets captured per bookmark

| Artifact | Source | Purpose |
|---|---|---|
| `final.png` | backbuffer readback post-tonemap, pre-UI | the headline golden |
| `gbuffer_albedo.png`, `gbuffer_normal.png` | MRT attachments (GBuffer0/1 — the same targets the chunk terrain writes, `terrain_lod_chunk.frag` MRT) | localizes *which stage* regressed: albedo diff = texturing/splat, normal diff = geometry/lighting inputs, final-only diff = lighting/post |
| `depth_vis.png` | linearized reverse-Z depth as grayscale | catches depth-write bugs (the 10.3 transparency saga — `glDepthMask` inheritance — would have lit up here instantly) |
| `state.json` | the `MC2_DEBUG_STATE_DUMP` snapshot taken at the capture frame | joins pixels to render state; consumed by `mc2-render-state` MCP tools |
| `render_counts.json` | RENDER_SNAPSHOT counters + per-pass draw counts at the capture frame | the §6 pre-gate input |

GBuffer/depth captures are **on-demand tier** (captured for goldens and for failing bookmarks only) — `final.png` alone for the routine advisory run, to keep run time and storage flat.

### 3.2 Storage

Goldens are **too big for git** (~25 bookmarks × 5 images × 5 missions ≈ hundreds of MB over time). Layout:

```
A:/Games/mc2-goldens/                      # outside the repo, like mc2srcdata
  sets/<set-id>/                           # one BLESSED SET, immutable once blessed
    set.json                               #   manifest: git describe, exe sha256+mtime,
                                           #   deploy target (v0.4 vs 0.4c — the stale-exe trap),
                                           #   bookmark file hashes, capture tuple, blesser, date
    mc2_01/overview_spawn/{final.png, gbuffer_*.png, depth_vis.png, state.json, render_counts.json}
    ...
tests/visual/                              # IN GIT (small, reviewable)
  bookmarks/<mission>.json
  masks/<mission>/<bookmark>.png           # nondeterminism masks (§5) — small, must be reviewed
  golden-sets.json                         # registry: set-id → path, status (blessed/candidate/retired),
                                           #   blessed_commit, supersedes
```

- **Versioning model = blessed sets, append-only.** A set is captured as `candidate`, reviewed (human walks the gallery), then `blessed` by a commit to `golden-sets.json` citing the run. Old sets are `retired`, never deleted (disk is cheap; provenance isn't).
- **Re-blessing** is required after any *intentional* visual change (e.g. terrain chunk cutover `a7b090be`). The bless commit message must name the change that justified it — same discipline as `tests/smoke/baselines.json` updates.
- The `set.json` exe-identity block exists specifically to kill the stale-exe false-alarm class (cockpit doc §6 identity-diff-first rule).

---

## 4. Diff metrics + pass/fail rules

Layered metrics, cheapest first; each layer can short-circuit PASS:

1. **Byte-identical?** PASS, done. (Achievable goal for in-engine readback + frozen sim on the same GPU/driver.)
2. **Per-pixel channel epsilon:** max per-channel delta ≤ 2 LSB after mask → PASS. Absorbs driver/FMA jitter without perceptual machinery.
3. **Changed-pixel ratio:** fraction of unmasked pixels exceeding epsilon. Thresholds: ≤0.05% PASS, 0.05–1% WARN (advisory, human review), >1% FAIL (advisory-FAIL until promotion; always human review).
4. **Perceptual score (FLIP):** NVIDIA FLIP (pip-installable `flip-evaluate`, designed for exactly this — rendering-algorithm comparison) computed on WARN/FAIL bookmarks only. Mean-FLIP ≤ 0.02 can downgrade a WARN to PASS-with-note (e.g. dither pattern phase shift); FLIP never upgrades severity. FLIP's error heatmap is saved as the third image in the review triptych.

Per-bookmark overrides live in the bookmark JSON (`"thresholds": {...}`) — a fog-heavy `mc2_24` bookmark legitimately needs a looser epsilon than a static rock closeup.

**Verdict record** (`visual_diff.json`, one per run): per bookmark `{verdict, changed_pct, max_delta, flip_mean, masked_pct, golden_set, paths{golden,current,diff,flip}}` + a run-level rollup `{pass, warn, fail, vacuous}`. Exit codes: 0 all-pass / 1 warns / 2 fails — agent contract.

---

## 5. Known nondeterminism — settle first, mask second

| Source | Strategy |
|---|---|
| Animated water (UV scroll, GPU-driven water sim) | **Settle:** pin animation clock (§2.3). If pinning the GPU water path proves invasive, **mask** water regions per bookmark. |
| gosFX / GPU particles | **Settle:** sim freeze + `MC2_DISABLE_GOSFX=1` / `MC2_GPU_PARTICLES=0` in the capture env (already smoke-allowlisted, VISUAL-CAPTURE-MATRIX-1). FX correctness has its own oracle lane (`docs/vfx-oracle-coverage.md`); the lab does not chase particles in pixels. |
| TAA / temporal jitter | **Settle:** capture preset forces non-temporal AA (or AA off). If TAA becomes default, add a `MC2_TAA_FREEZE_JITTER=1` capture knob. |
| Mover animation / AI | **Settle:** sim freeze. Idle-spawn bookmarks additionally avoid movers in frame where possible. |
| Light flicker / time-of-day | **Settle:** clock pin. |
| Mouse cursor / UI overlays | **Settle:** capture pre-UI (readback before interface draw), cursor not rendered in smoke mode. |
| Driver/GPU variance (this is a one-machine lab: 7900 XTX) | **Accept:** 2-LSB epsilon. Goldens are per-machine; `set.json` records GPU+driver; a driver update invalidates sets (re-bless, diff old-vs-new as a driver-change audit). |

**Masks** are per-bookmark grayscale PNGs (`tests/visual/masks/`, white = compared, black = ignored), authored by capturing the same bookmark twice with deliberately advanced clocks and dilating the diff. Masks are in git and reviewed: a growing mask is a smell (it hides regressions) — `visual_diff.json` reports `masked_pct` and the lab WARNs if any mask exceeds 15% of frame.

---

## 6. Oracle pre-gates — numbers before pixels

Pixel diffing is the *last* resort, not the first. Per bookmark, before any image comparison:

1. **RENDER_SNAPSHOT ok-gate:** `render_counts.json` (golden) vs current — the ~13 mismatch/fail counters aggregated in `RenderSnapshot::ok` (`GameOS/gameos/render_snapshot.h:195-204`) must all be 0 in *both*. Nonzero current = the regression is already attributed to a subsystem; report it as the headline and skip/annotate the pixel diff.
2. **Composition diff:** per-pass draw counts, instance counts, triangle counts, bound-texture counts from the snapshot. `terrain draws 412→0` is an infinitely better first signal than "97% of pixels changed". This is the cheap detector for the whole "frozen GL state inheritance" bug class (10.3 saga).
3. **State-dump diff:** key fields of `state.json` (feature gates via `get_feature_gates`, visual settings via `get_visual_settings` — `scripts/mcp/mc2_render_state_server.py`). A gate-env mismatch between golden and current stamps the comparison `SUSPECT` (apples-to-oranges) before any pixels are read — extending the cockpit identity-diff-first rule to visual runs.
4. Only then: pixel layers (§4).

Output ordering in the verdict: `SUSPECT > oracle-fail > composition-delta > pixel-fail > pixel-warn > pass`. The cockpit Oracle Board (telemetry doc §5.2) gains a "Visual" row group fed by `visual_diff.json` — the lab's report **is** a cockpit artifact.

---

## 7. Artifact layout + browser

Extends the cockpit run-folder shape (`tests/smoke/artifacts/<timestamp>/`, telemetry doc §4):

```
tests/smoke/artifacts/<timestamp>/
  manifest.json, report.json, telemetry.ndjson, ...   # cockpit-owned, unchanged
  visual/
    visual_diff.json                       # run-level verdict (the agent reads THIS)
    <mission>/<bookmark>/
      current_final.png                    # what this build rendered
      diff_final.png                       # amplified abs-diff, magenta-on-gray
      flip_final.png                       # FLIP heatmap (WARN/FAIL only)
      current_gbuffer_*.png, current_depth_vis.png   # FAIL only
      render_counts.json, state.json
    index.html                             # the BROWSER (single self-contained file)
```

**Browser = single-file HTML gallery** (same v1 skin as the cockpit HTML report — zero runtime deps, generated by the diff tool):

- Grid of bookmark cards sorted FAIL → WARN → PASS → vacuous; pass cards collapsed by default. A clean run is a one-screen wall of green.
- Each FAIL/WARN card: **golden | current | diff** triptych with a hover/click A-B flicker toggle (the single most effective human diff primitive), the numeric verdict line, the oracle pre-gate summary, and a "stage attribution" line when GBuffer captures exist (albedo-clean+final-dirty ⇒ lighting/post).
- Header: identity block (exe, git describe, golden set id, env deltas) — SUSPECT banner if mismatched.
- v2: the editor/cockpit ImGui panel embeds the same data (telemetry doc Phase 4); the HTML never goes away.

The human workflow: agent posts "visual run: 2 FAIL, 1 WARN, open `<path>/visual/index.html`" — the user reviews three cards, not a game session.

---

## 8. Baseline A bootstrap — the first blessed set

Baseline A (MEMORY: golden frames / per-pass timings / oracle counters off `mc2-win64-0.4c`, post-terrain-8z, pre-GlStateGuard) is exactly the lab's genesis event:

1. **Author bookmarks first** (Slice 2) against the verified 0.4c build — ~25 bookmarks across tier1 missions, user-assisted (user flies, hotkey dumps `[BOOKMARK v1]`, agent assembles JSONs). This is the only step needing human eyes-on-game.
2. **Capture candidate set** `baselineA-0.4c` with the in-engine readback path: final + GBuffer + depth + state + counts for every bookmark. Also capture it **twice** back-to-back — the self-diff must be byte-identical (or ≤2 LSB) per bookmark; any bookmark failing self-diff gets settled/masked/dropped before blessing. *Self-stability is the entry criterion for the set.*
3. **Bless:** human walks `index.html` of the self-diff run, then commit `tests/visual/golden-sets.json` pointing at `A:/Games/mc2-goldens/sets/baselineA-0.4c/` with the exe identity block. This same run folder is the cockpit's `baseline_run` (telemetry doc §6/§7) — one blessing, two consumers (pixels + budgets).
4. From then on: GlStateGuard work, Tube merge, A2/A4 deferred-work, and every shader slice diff against `baselineA-0.4c` and re-bless only on intentional change.

---

## 9. CI / smoke integration — advisory first, gate later

- **Phase A (advisory):** `py -3 scripts/run_visual.py --set baselineA-0.4c [--covers terrain]` is a *separate* invocation from `run_smoke.py` — never inside the tier1 inner loop (tier1 stays <3 min; a full visual run is ~5 min of teleport+settle). Slice-completion checklists add "visual run: 0 FAIL or diffs human-acked".
- **Run-folder unification:** visual runs write the standard artifact folder + manifest so the cockpit indexes them alongside smokes.
- **Phase B (selective gating):** after ≥3 weeks / ≥20 runs of a bookmark+metric being stable (no flaky WARNs), promote per the cockpit §8 rule: add a `visual` bucket to `smoke_lib/gates.py` that reads `visual_diff.json` *iff present* — absence is never a failure (the no-mandatory-step rule). Hard-gate only layer-1/2/3 metrics, never FLIP.
- **Concurrency:** visual runs hold the same run_smoke lock (no concurrent mc2.exe — existing `run_smoke.py` lock discipline; never `--kill-existing`).

---

## 10. Anti-goals (binding)

- **No OS-compositor screenshots in verdicts.** `pyautogui` paths (`camera_sweep_smoke.py`, `game_auto.py`; capture_baseline.py was retired 2026-10-12, cull wave 1) remain for ad-hoc human use; the lab never grades them.
- **No perceptual-metric-only gates.** FLIP downgrades, never decides alone — heatmaps are for humans.
- **No video / scripted-path diffing in v1.** Discrete poses only; spline fly-through diffing is a someday-maybe.
- **No multi-machine golden portability.** One machine, one GPU, per-machine sets. Cross-GPU is out of scope until there is a second machine.
- **No goldens in git.** Only bookmarks, masks, thresholds, and the set registry.
- **No second verdict engine, no agent pixel interpretation.** Agents read `visual_diff.json`; gates.py stays sovereign; humans look at images.
- **No new engine network/IPC.** Capture rides smoke-mode env vars + file output, like everything else.

## 11. Risks

| Risk | Mitigation |
|---|---|
| Settle rules don't fully tame water/FX → flaky diffs erode trust | Self-diff entry criterion (§8.2); masks as escape hatch with `masked_pct` ceiling; flaky bookmarks demoted to `vacuous` not deleted silently |
| In-engine readback slice stalls (PBO/format/sRGB pitfalls) | Capture path is read-only + env-gated, off by default; fall back to readback-at-vsync simple `glReadPixels` (perf irrelevant — it runs 25×/run, not per frame) |
| Sim-freeze flag perturbs the very state being captured (heisen-capture) | Freeze only the update tick, never render; verify via composition diff freeze-on vs freeze-off at one static bookmark |
| Golden rot — intentional changes ship without re-bless, lab becomes red noise | Re-bless required by slice checklist; SUSPECT stamping on identity mismatch; `golden-sets.json` is git-reviewed |
| Disk growth of retired sets | Retired sets keep `final.png` + manifests only (GBuffer/depth pruned); sets/ on the asset drive, not the repo |
| Driver update silently shifts pixels machine-wide | `set.json` records driver version; mismatch ⇒ SUSPECT; re-bless after deliberate driver-change audit run |
| Bookmark authoring stalls on user availability | Slice 2 ships a fallback: auto-bookmarks at mission spawn + 3 deterministic offsets, replaced by curated poses incrementally |

## 12. Phased roadmap

- **Phase 0 — Capture primitive.** In-engine readback (`MC2_VISUAL_CAPTURE`), bookmark JSON loader + teleport in `gos_smoke.cpp` harness, settle rules, state/counts sidecars.
- **Phase 1 — Diff + verdict.** `scripts/visual_diff.py` (layers 1–3 + masks), `visual_diff.json`, exit codes, `index.html` gallery.
- **Phase 2 — Baseline A bless.** Bookmark authoring session, self-diff stabilization, `golden-sets.json`, first blessed set (= cockpit baseline_run).
- **Phase 3 — Oracle pre-gates + FLIP.** render_counts/state pre-gate ordering, FLIP layer, GBuffer/depth on-demand tier, cockpit Oracle Board "Visual" rows.
- **Phase 4 — Workflow integration.** `--covers` filtering, slice-checklist advisory step, editor bookmark authoring button, cockpit ImGui embedding.
- **Phase 5 — Promotion.** Stable bookmarks promoted into a gates.py `visual` bucket per the cockpit §8 rule.

## 13. First 5 implementation slices

1. **S1 — In-engine capture:** `MC2_VISUAL_CAPTURE_FRAME=<n>` + `MC2_VISUAL_CAPTURE_DIR` → at frame n, glReadPixels backbuffer → PNG (stb_image_write, already vendored) + state dump + RENDER_SNAPSHOT counters JSON. No bookmarks yet — proves byte-stability of the readback path vs the OS-grab path on one tier1 mission, twice.
2. **S2 — Bookmark replay:** bookmark JSON schema + loader, smoke-mode teleport/settle/capture loop over a hand-written 2-bookmark `mc2_01.json`; `[BOOKMARK v1]` hotkey dump for authoring. Self-diff twice → byte-compare.
3. **S3 — visual_diff.py + gallery:** layers 1–3, masks, `visual_diff.json`, exit 0/1/2, single-file `index.html` triptych gallery. Golden-test against S2's pair of runs (must PASS) and against a deliberately broken run (`MC2_TERRAIN_LOD_CHUNK=0` legacy path — must FAIL loudly).
4. **S4 — Baseline A authoring + bless:** user-assisted bookmark session across tier1 5, capture `baselineA-0.4c` candidate, self-diff stabilization (settle/mask), `tests/visual/golden-sets.json` + bless commit.
5. **S5 — Oracle pre-gate:** render_counts/state.json comparison ordered before pixels, SUSPECT stamping on identity/gate-env mismatch, verdict ordering per §6; wire `visual_diff.json` into the cockpit oracle_summary consumers.

## 14. Follow-up prompts for Opus/Codex

- "Implement S1 from `docs/superpowers/strategy/visual-regression-lab-architecture.md`: env-gated in-engine backbuffer readback to PNG in the `MC2_SMOKE_MODE` harness (`GameOS/gameos/gos_smoke.cpp`), plus state-dump + RENDER_SNAPSHOT counter sidecars at the capture frame. Zero cost when env unset; prove byte-stability by capturing mc2_01 frame 900 twice on the same exe and `fc /b`-comparing. Do not touch run_smoke.py verdict paths."
- "Design the bookmark teleport mechanics for S2: given `code/gamecam.cpp`'s Camera (position/rotation/projection/zoom), determine the minimal hard-set API that bypasses input smoothing and terrain-following, what must settle afterward (chunk LOD admission frame N−1 MVP, shadow warmup, texture streaming), and propose the settle-frame count with evidence from two-capture self-diffs at 10/30/60 frames."
- "Implement S3 (`scripts/visual_diff.py` + index.html gallery) per §4/§7 of the lab doc: numpy-based layers 1–3 with per-bookmark mask PNGs and threshold overrides, `visual_diff.json` with exit codes 0/1/2, and a self-contained HTML triptych gallery with A/B flicker toggle. Golden-test on the S2 artifact pair; verify FAIL detection by diffing chunk-terrain vs `MC2_TERRAIN_LOD_CHUNK=0` legacy captures."

---

*Grounding index: capture tuple + tier1 fixture rationale `docs/VISUAL-CAPTURE-MATRIX-1-DESIGN.md`; smoke harness `scripts/run_smoke.py` + `scripts/smoke_lib/` (gates.py verdict authority); smoke-mode env plumbing `GameOS/gameos/gos_smoke.cpp` (`MC2_SMOKE_MODE`/`MC2_SMOKE_SEED`); existing OS-grab prototypes `scripts/capture_baseline.py`, `scripts/camera_sweep_smoke.py`, `scripts/game_auto.py`; oracle counters `GameOS/gameos/render_snapshot.h` + `docs/oracle-dynamic-pipeline-gate.md`; agent access `docs/mcp-render-state.md` (`MC2_DEBUG_STATE_DUMP=1`, run_capture_baseline/list_capture_sets/summarize_latest_capture); cockpit contract `docs/superpowers/strategy/telemetry-oracle-cockpit-architecture.md` §4 (run folders), §6 (identity-first compare, baseline_run), §8 (severity/promotion).*
