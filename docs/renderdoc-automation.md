<!-- doc-status: current | reviewed:2026-10-11 -->
# RenderDoc draw-call attribution automation

`tools/renderdoc/` turns the manual "which draw painted these pixels?" hunt
into one capture plus one command. It exists because the black-band
investigation cost ~13 build cycles of tint instrumentation to answer a
question a single RenderDoc capture + a pixel-history walk answers directly:
*which draw wrote the top strip, and what is its blend state?*

This is the **attribution / analysis** layer. It is complementary to, and does
not duplicate, the two RenderDoc pieces that already shipped:

- **Engine hook** — `GameOS/gameos/gos_rdoc_capture.cpp` (Sketch C, in-process
  `TriggerCapture()` via the `MC2_RDC_*` env gates). See
  [docs/testing-strategy-engine-hooks.md](testing-strategy-engine-hooks.md)
  "Sketch C -- Tier 5" for the design and the `renderdoc_app.h` vendoring.
- **Parity orchestrator** — `scripts/renderdoc_capture.py` (launch → capture →
  `renderdoccmd convert` → normalized pipeline-state diff for regression).
  See [docs/testing-strategy.md](testing-strategy.md) Tier 5.

Those answer *"did the pipeline state change between builds?"*. This tool
answers *"which draw is responsible for these pixels, right now, and what is
its state?"* — the attribution question.

## Requirements

RenderDoc must be installed. The tools auto-discover `qrenderdoc.exe` /
`renderdoccmd.exe` on `PATH`, at `C:/Program Files/RenderDoc`, or any
`C:/Program Files*/RenderDoc*` dir; override with `--qrenderdoc` /
`--renderdoccmd`.

The replay analysis (`attribute.py`, `states.py`) runs the worker
`rd_worker.py` **inside** `qrenderdoc.exe --python`. The stock Windows
RenderDoc build ships **no** standalone `renderdoc.pyd` for an external
interpreter, so `qrenderdoc --python` is the supported headless entry point
(verified on v1.44). The worker `os._exit`s before the Qt UI opens — no window,
no hang, ~1–3 s per capture. If RenderDoc is absent, the tools fail with a
clear message and `capture.py --dry-run` still validates the launch plan.

## Tools

### `capture.py` — launch the game and save a posed frame

Launches `mc2.exe` under `renderdoccmd capture` (injecting `renderdoc.dll`),
drives the engine's `MC2_RDC_*` hook to trigger at a target frame, and — the
part the parity orchestrator lacks — pins the camera to a **known pose** via
the game's bookmark/park fixtures so the captured frame is deterministic.

```
py -3 tools/renderdoc/capture.py \
    --install A:/Games/mc2-opengl/mc2-win64-v0.4 \
    --mission mc2_10 \
    --bookmark tests/visual/bookmarks/mc2_10.json \
    --frame 120 \
    --out tests/smoke/captures/mc2_10_frame120.rdc
```

- `--bookmark <json>` → `MC2_VISUAL_BOOKMARK_CAPTURE` (+ aligns
  `MC2_VISUAL_CAPTURE_FRAME` with `--frame`).
- `--park x,y,rot,proj,alt[@frame]` → `MC2_CAMERA_PARK` (one-shot pose pin).
- A **sequential game-run lane guard** (own-PID lockfile
  `tools/renderdoc/.game-run.lock`) prevents two agents launching the game at
  once; `--no-lane` opts out. **Lane discipline: never launch a capture while
  another `mc2.exe` is running** — check `tasklist` first.
- `--dry-run` prints the full launch plan without launching (validates
  renderdoccmd/exe discovery and env even where RenderDoc or the install is
  absent).

### `attribute.py` — pixels → draws

For a screen region, samples an `NxN` grid of pixels and walks RenderDoc's
per-pixel history to report every draw that touched them, ordered so the
draw that actually **wrote** the pixels (the visible top contributor) is
first. Per draw: call name, pass/marker context, vertex/instance counts,
VS/PS program names, blend state, depth test/write, cull.

```
py -3 tools/renderdoc/attribute.py \
    --rdc tests/smoke/captures/mc2_10_frame120.rdc \
    --region 0,0,1920,60          # x,y,w,h in backbuffer px; omit = whole frame
```

Emits `<rdc>_attrib.md` and `<rdc>_attrib.json` (override with `--out` /
`--json`). `--grid N` sets the sample density (default 11 → 121 samples).

### `states.py` — one draw, full pipeline state

Dumps the complete pipeline state for a single `eventId` (the `eid` column
from `attribute.py`): all shader stages + entry points + bound textures,
**color blends per render target** (the "is the opaque cone additive?"
question in one command), depth/cull, topology, viewport/scissor, output
targets.

```
py -3 tools/renderdoc/states.py \
    --rdc tests/smoke/captures/mc2_10_frame120.rdc --draw 1115
```

Emits `<rdc>_draw<id>.md` / `.json`. If `--draw` is not itself a drawcall, the
nearest drawcall at/after it is used.

## Worked proof (the black-band pattern)

Against a real capture (`tests/smoke/captures/mc2_01_probe_frame61.rdc`),
attributing the top strip `0,0,1920,60` reports a **single** contributing draw:

| eid | draw | wrote/hit | verts | blend | depth | cull |
|-----|------|-----------|-------|-------|-------|------|
| 1115 | `glDrawArrays()` | 121/121 | 6 | OFF (writeMask=RGBA) | test=off write=off | NoCull |

`states.py --draw 1115` confirms the signature of a **full-screen overlay
quad**: 6-vertex `TriangleList`, blend OFF on all 8 RTs, depth test/write off,
no cull. That 6-vert / no-depth / blend-off draw *is* the `gos_vertex` overlay
blit — exactly what the manual tint hunt was chasing, produced in one command.

Note: OpenGL programs here carry RenderDoc's auto-names (`Shader 222`) because
the engine does not `glObjectLabel` its programs. Adding `glObjectLabel` to the
GL program objects would surface engine-side names (e.g. `gos_vertex`) directly
in these reports — a cheap, high-value follow-up.

## Notes / troubleshooting

- **Backbuffer coordinates.** Regions are in the sampled target's pixel space
  (the backbuffer, reported in every attribution header — e.g. 1920×1080).
- **No `.rdc` produced by `capture.py`.** The frame-solid latch may not have
  armed or the frame count was never reached — raise `--duration`, confirm the
  mission actually renders, and check the `MC2_RDC_*` env in the run log.
- **Multi-draw indirect.** `glMultiDraw*Indirect` sub-draws each get their own
  `eventId`; attribution resolves to the exact sub-draw that wrote the pixel.
