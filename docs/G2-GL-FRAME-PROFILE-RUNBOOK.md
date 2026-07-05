# G2 — GL frame profile runbook (DEMOTED — no longer a Vulkan gate)

> **★ RULING (2026-07-03, user): Vulkan is for FEATURES, not performance.**
> The motivation is capabilities GL cannot provide — **multithreaded command
> submission** and **ray tracing (`ray_query`)** — not FPS. So the CPU-vs-GPU
> profile is **NOT a go/no-go gate**; it never was the right question.
> - CPU-bound at zoom (the likely reality) *argues FOR* Vulkan — multithreaded
>   submission is the direct fix, not a reason to stay hybrid.
> - RT is GL-impossible → a capability spike, not a perf decision.
> - G1 (zero-copy interop) already passed → add Vulkan **islands** for specific
>   features on the shared image, keep GL elsewhere. Hybrid-by-design.
>
> **G2 is therefore OPTIONAL and downgraded to a SIZING tool** — a Tracy CPU-zone
> capture at wolfman zoom to localize where CPU frame time goes (submission /
> renderLists flush / Camera.UpdateRenderers), so the eventual multithreaded-
> submit slice knows what to thread first. Run it if/when that slice starts.
> **The real next Vulkan step is G3: an RT contact-shadow (`ray_query`) island
> spike on the G1-proven interop image.** Nothing is frozen on this profile.

**Original goal (kept for the sizing use):** measure the current GL frame —
CPU frame time, GPU frame time, per-pass GPU breakdown.

## Can this be done fully headless / via CLI?

**No — not for the GPU numbers.** GPU per-pass timing must be read through a
GPU tool's UI (RGP, RenderDoc, or Tracy). What *is* CLI-doable:

- **Capturing** a RenderDoc `.rdc` headless — proven working:
  `renderdoccmd capture -w <exe> --mission mc2_01 --duration 120 --smoke-active`
  with `MC2_SMOKE_MODE=1` (advances frames without a human) + `MC2_RDC_CAPTURE_FRAME=N`.
  (Use the space-free `mc2-win64-v0.4` slot; `renderdoccmd` chokes on the space
  in "0.5 testing".) The `.rdc` *contains* per-draw GPU durations — but reading
  them needs the RenderDoc UI (the `renderdoc` python module is not importable
  in the agent env, and `renderdoccmd` doesn't print counters).
- **Frame image** headless: `renderdoccmd thumb <rdc> -o out.png` (used this to
  debug the overlay double-draw).

So: **the capture can be scripted; the GPU-time read is interactive.** Pick one
of the three tools below.

> ⚠ **Measure at "wolfman zoom" with real input.** The smoke harness sends no
> input, so smoke FPS is deceptive and NOT comparable. Drive the camera down to
> a normal combat zoom on a populated mission (mc2_24 / wolfman) before capturing.
> (See `docs/render-perf-snapshot.md`.)

---

## Option A — Tracy (RECOMMENDED: one view, CPU **and** GPU)

The engine is already Tracy-instrumented on both sides: `TracyGpuContext` +
`TracyGpuCollect` (gameosmain.cpp) and `TracyGpuZone` on the real passes
(`Terrain::IndirectDraw`, `renderWaterFastPath`, `DynamicShadowPass`, …). Tracy
runs **on-demand** (TCP 8086) — zero cost until a client attaches.

1. Get the Tracy profiler GUI (`Tracy.exe` / `tracy-profiler`, v0.10-ish to
   match the vendored client; check `3rdparty/tracy`).
2. Launch the game normally (windowed, focused — NOT minimized):
   `run-mc2.bat` (or `mc2.exe -mission mc2_24`). No special env needed; Tracy is
   always compiled in.
3. In Tracy: **Connect** to `127.0.0.1:8086`.
4. In-game, drive to wolfman zoom on a busy moment (units + terrain + FX on
   screen). Let it run a few seconds so Tracy accumulates frames.
5. Read, per frame:
   - **CPU frame time** = the top-level frame zone (or `Frame` markers) width.
   - **GPU frame time** = the GPU track's total per frame (Tracy shows a
     separate GPU timeline populated by `TracyGpuCollect`).
   - **Per-pass GPU** = the `TracyGpuZone` bars (`Terrain::IndirectDraw`,
     water, shadow, post) — this is the breakdown that tells you where GPU time
     goes.
6. **Verdict:** if GPU frame time ≈ CPU frame time and both ≈ the frame
   interval → GPU-bound. If CPU zone total ≫ GPU total → CPU-bound (and the
   Vulkan-for-perf argument weakens — the win would be on the CPU submission
   side, not raw GPU).

---

## Option B — RGP (deepest AMD GPU profile; 7900 XTX)

Best hardware-level GPU detail (per-event GPU duration, occupancy, wavefronts,
stalls). GUI-driven via Radeon Developer Panel.

1. Launch **Radeon Developer Panel** (Radeon Developer Tool Suite). Enable
   profiling for the target exe.
2. Launch `mc2.exe` (the Panel injects the RGP layer).
3. Drive to wolfman zoom, then hit the **RGP capture hotkey** (default
   `Ctrl+Shift+C`).
4. Open the `.rgp` in Radeon GPU Profiler. Record: total GPU frame time, the
   per-event/per-pass GPU duration, and occupancy on the terrain/shadow/post
   passes. Cross-check the pass names against the Tracy GPU zones.

RGP gives GPU only — pair it with Tracy (Option A) or the engine's CPU logs for
the CPU side.

---

## Option C — RenderDoc (CLI capture + UI read of GPU duration)

Use when you want a single deterministic frame and the exact per-draw GPU cost.

1. Capture headless (scriptable):
   ```
   py -3 scripts/renderdoc_capture.py capture --mission mc2_24 --frame 120 \
      --out A:/Games/mc2-opengl/mc2-win64-v0.4/_rdoc/g2.rdc --skip-convert
   ```
   (deploy your build to `mc2-win64-v0.4` first; env `MC2_SMOKE_MODE=1` is set
   by the harness). NOTE: the smoke camera is not wolfman zoom — for a
   representative frame, capture from a live windowed run with the in-process
   hook (`MC2_RDC_CAPTURE_FRAME=N`) after driving to zoom, or accept the smoke
   framing as a lower-bound.
2. Open the `.rdc` in the **RenderDoc UI** → Event Browser → enable the **GPU
   Duration** column (or Window → Counter Viewer, `GPU Duration`). Read total +
   per-draw. Group by the terrain/shadow/post markers.

---

## What to record (the G2 deliverable)

For a wolfman-zoom frame on mc2_24 (and ideally one more busy mission):

| Metric | Value |
|---|---|
| CPU frame time (ms) | … |
| GPU frame time (ms) | … |
| Frame interval / FPS | … |
| Top GPU passes (ms) | Terrain::IndirectDraw …, shadow …, water …, post … |
| Top CPU zones (ms) | Camera.UpdateRenderers …, GameLogic.* … |
| **Verdict** | CPU-bound / GPU-bound / balanced |

**Then the Vulkan branch (per the arc):**
- interop works (G1 ✅) **and** GPU-bound with headroom to win → resume broader
  Vulkan *only* with written kill criteria.
- GPU-bound but the win doesn't justify a full-frame port → minimal Vulkan only
  for GL-impossible features (RT contact shadows via `ray_query`).
- CPU-bound → the Vulkan-for-GPU-perf premise is wrong; land at hybrid, spend
  effort on CPU submission instead.

Paste the table back and I'll do the branch analysis + next-step scoping.
