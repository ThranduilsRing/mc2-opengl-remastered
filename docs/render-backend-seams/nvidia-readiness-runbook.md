<!-- doc-status: current | reviewed:2026-10-11 -->
# NVIDIA-READINESS-AUDIT-1 — first-NVIDIA-run runbook

Readiness-ONLY. This is not a fix and not seam governance — it is a pre-flight so the first
real NVIDIA run is **surgical, not exploratory**. All seam work to date was AMD-tested only;
NVIDIA differs in driver tolerance, implicit-sync behavior, shader-compiler strictness, and
debug output. **Do not "fix NVIDIA from AMD."** Pack the net, label the traps, aim the run.

Source state: nifty `f432e99e`. 8 render-seam checkers PASS on tree (static gate, below).

## 1. Known NVIDIA-gated risks (cannot be proven on AMD)

| Risk | Where | Symptom to watch on NVIDIA |
|---|---|---|
| **lightData_ orphan/grow** | `gameos_graphics.cpp` LIGHTSSBO-ORPHAN-1 (~:8799/8918), gate `MC2_GPUBUF_LIGHT_GROWONCE` (default OFF) | `RenderLists.LightDataUpload` Tracy spike (~80 ms was seen on a 1050 Ti before orphan). The orphan exists to dodge this — verify it still does on the target NVIDIA card. |
| **Class-D implicit sync** | particle/tube SSBOs (gated `MC2_GPU_PARTICLES`, default OFF), terrain-mask indirect, `lightData_` | Per-frame stalls from `glBufferSubData` into in-flight buffers. Particle path is DORMANT by default → not a first-run risk unless `MC2_GPU_PARTICLES=1`. |
| **Shader compiler strictness** | GLSL via `makeProgram` + optional SPIR-V (`MC2_SHADER_SPIRV`, default OFF) | NVIDIA GLSL compiler rejects things AMD accepts (implicit casts, unused-binding layout). Watch shader compile/link diagnostics (`MC2_SHADER_COMPILE` tag). |
| **Debug-output-only errors** | GL debug callback | NVIDIA emits warnings/errors AMD silently ignores. `MC2_GL_DEBUG_FATAL=1` converts them to hard stops — expect *new* messages; triage before assuming regression. |
| **State leakage AMD tolerates** | tex-unit / draw-buffer / colorMask / viewport leaks | Already guarded (GlScopedTextureUnit, colorMask keystone, draw-buffer chokepoint, save/restore). NVIDIA is the audience that would *show* an unguarded leak. |
| **Release vs RelWithDebInfo** | per CLAUDE.md | Release crashes on GL debug callback registration — **always RelWithDebInfo on NVIDIA too.** |

## 2. First-run gate profile (do NOT enable everything forever)

**Functional sanity profile** (one autoplay smoke pass):
```
MC2_GL_DEBUG_FATAL=1          # NVIDIA will surface AMD-tolerated GL errors as hard stops
MC2_RENDER_FRAME_PLAN_TRACE=1 # confirms live pass/path identity matches AMD
MC2_MDI_SUBMIT_TRACE=1        # indirect submitters fire as on AMD
MC2_PIPELINE_BIND_TRACE=1     # pipeline binds present (terrain/static-prop/shadow/post-fx)
```
**Byte-parity profile** (separate, no debug-fatal so a benign NVIDIA warning doesn't abort the capture):
```
(deterministic capture defaults) — compare frame hashes to the AMD reference (§4)
```
**Light-stall profile** (only if investigating the orphan): `MC2_GPUBUF_COUNTER=1` + Tracy
`RenderLists.LightDataUpload`. Do NOT set `MC2_GPUBUF_LIGHT_GROWONCE=1` on the first run —
that's the change we're trying to validate, not the baseline.

Run profiles separately. Debug-fatal + full traces + capture all at once produces noise.

## 3. Mission smoke matrix (covers the governed paths)

| Mission | Covers |
|---|---|
| **mc2_24** | postfx family + terrain + water + most static props/shadows (baseline) |
| **mc2_10** | static props in frame + gosFX-capable mission |
| mc2_03 (optional) | alternate prop/terrain coverage |

Default the functional pass to **mc2_10 + mc2_24** (the standing inner-loop pair; covers
props + postfx + terrain + water). Add mc2_03 only if a prop/terrain anomaly appears.
Do NOT run a particle-heavy fixture unless `MC2_GPU_PARTICLES` is intentionally enabled
(it is default-OFF and visually WIP — not a readiness target).

## 4. AMD reference to match (byte-parity anchor)

Deterministic capture (`run_visual_capture.py`, fixed clock) on AMD produced these stable
frame hashes (gate-OFF, from COLORMASK-ROLLOUT-POSTFX-1 A/B):
```
mc2_01 overview_center  = 2d4a049db4a4
mc2_01 highangle_wide   = 262bc8f931eb
```
First NVIDIA capture of the same bookmarks: **hashes will almost certainly DIFFER** (vendor
rasterization/filtering differences are expected) — that is NOT a regression by itself. The
useful test is **NVIDIA-vs-NVIDIA stability** (same hash across runs) + **visual A/B** vs the
AMD screenshots, not a cross-vendor byte match. Re-bless a NVIDIA golden set; don't force
the AMD hash.

## 5. Expected/diagnostic vs real regression

| Observation | Verdict |
|---|---|
| New GL debug warnings under `MC2_GL_DEBUG_FATAL=1` | EXPECTED first — triage each; many are AMD-tolerated benign. Only a *write/read hazard* or *invalid-op* is real. |
| Frame hashes differ from AMD | EXPECTED (cross-vendor). Regression only if NVIDIA-vs-NVIDIA is unstable or visual is wrong. |
| `crash_silent` / 0-frames on one mission of a batch | Usually environmental (concurrency/launch), as on AMD — re-run isolated before believing it. |
| `gpu_drawn_instances=0` | EXPECTED — declared stale (legacy-path-only; see ledger). Read `submitted_instances`. |
| `RenderLists.LightDataUpload` spike | REAL signal — the lightData_ NVIDIA stall the orphan defends against. Capture the Tracy zone. |
| Shader compile/link FAIL that AMD passed | REAL — NVIDIA stricter GLSL; fix the shader (the one legit "fix on NVIDIA evidence" case). |

## 6. NVIDIA-only HOLD list (do not implement until NVIDIA evidence)

- **lightData_ ring / grow-once-fence (`MC2_GPUBUF_LIGHT_GROWONCE`)** — HOLD; the whole point
  is the NVIDIA stall verdict. Don't enable/ship from AMD.
- **Class-D particle barrier/ring** — HOLD (bridge default-OFF + WIP; re-measure only if
  `MC2_GPU_PARTICLES` becomes shippable, via the in-tree probe).
- **VIEWPORT-GUARD-1 / TEXTURE-TARGET-UNIT-CHECKER-1** — optional polish, NOT NVIDIA blockers.

## 7. Logs/artifacts to capture on the first NVIDIA run

- Full stderr (`MC2_LOG=1`, redirect like `run-with-log.bat`) — keep the whole file.
- `[BUILD_FINGERPRINT]` line (confirm the built sha + RelWithDebInfo).
- All `[FRAME_PLAN]`, `[PIPELINE_BIND]`, `[MDI_SUBMIT]` lines — diff the *set* of passes/
  pipelines/submitters against an AMD log (presence parity, not timing).
- Any GL debug-callback messages (under `MC2_GL_DEBUG`/`_FATAL`).
- `[OBJBATCHER v1]` summary (submitted_instances sane, fallback_rate low).
- Deterministic capture PNGs/hashes for the §3 missions → bless a NVIDIA golden set.
- Tracy capture if investigating the light stall.

## Static readiness gate (verified on tree, AMD)

All 8 render-seam checkers PASS at `f432e99e`: `colormask_ownership`, `pass_attachment`,
`frame_feedback`, `drawbuffer_ownership`, `static_prop_family`, `mdi_submission`,
`buffer_lifetime`, `shader_variant`. Re-run `scripts/check-contracts.sh` (or
`bash scripts/check-contracts.sh --rows 'vulkan_bindings|vk_pipeline_layout|vk_shaders_compile'`; the run_nvidia_readiness_smoke.py wrapper was retired 2026-10-12, cull wave 1) on the NVIDIA checkout before the
first run — the checkers are vendor-independent and must stay green.

## Do NOT (readiness boundary)
No light ring on AMD · no class-D barriers · no buffer-upload rewrite · no NVIDIA-specific
code paths added blind · no GPU-particles-as-readiness unless shipping them. Actual NVIDIA
fixes wait for actual NVIDIA evidence.
