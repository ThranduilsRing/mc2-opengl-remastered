# CAMERA-SMOOTHING-1 — status (2026-07-22)

## State: code COMPLETE + committed + built; DEPLOY PENDING (lane busy all evening)

Branch `claude/deferred-sweep-1`, worktree A:/Games/mc2-deferred-1.

Commits (oldest first):
- `4b11c7e1` feat: frame-rate-independent camera smoothing (MC2_CAMERA_SMOOTH, default 100ms)
- `c238d1d7` docs: gate registry + smoke passthrough + tier1 docs  ← **DEPLOYED** (user's live session runs this)
- `50b04d79` feat: rotation focus — continuous held-rotate feed (bypasses 11.25° quantizer when smoothing on)
- `c2f7670b` fix: rebase (not drop) smoothing goals on external camera motion
- flags.auto.yaml regen (HEAD)

Build at HEAD is in `build64/RelWithDebInfo/mc2.exe` — **deploy when lane frees**:
`py -3 scripts/deploy_payload.py "A:/Games/mc2-opengl/releases/mc2-win64-deferred-test" --allow-new-target`
(the script's own running-exe guard refuses while the user plays — safe to attempt).

## Root cause of the jerk (user confirmed: ROTATION)

- **Rotation (the jerk):** `code/missiongui.cpp` rotateLeft/rotateRight — accumulator
  advances `degPerSecRot=6000` per `BaseFrameLength=15` (~400°/s) and fires an
  INSTANT `eye->rotateLeft(RotationIncrement=11.25°)` snap every time it crosses the
  threshold: one hard 11.25° jump every ~2-3 frames, interval varying with frame time.
- Scroll: hard per-frame position steps (missiongui scrollUp/... → Camera::moveX →
  instant setPosition), frame-time jitter maps 1:1 into position deltas.
- Zoom: already lerped but fixed-rate 20/s (not dt-independent).

## Knob

`MC2_CAMERA_SMOOTH=<ms>` — default **100** (unset), `0` = legacy verbatim, clamp [0,2000].
Also `MC2_CAMERA_SMOOTH_TRACE=1` (per-frame pose deltas → cam_smooth_trace.csv) and
`MC2_CAM_SMOOTH_TEST=1` (synthetic scroll burst frames 120-240, one 11.25° snap at 150,
held-rotate burst at legacy cadence frames 300-420).

## Evidence so far (run at c238d1d7, default 100ms, mc2_01 smoke PASS 1569 frames)

`.claude/captures/cam_trace_default100.csv`: 11.25° input snap at frame 150 →
drot per frame: 1.27, 1.22, 0.99, 0.95, 0.89, 0.67 ... exponential taper to 0 over
~40 frames (vs legacy: single-frame 11.25 jump). Scroll steady-state rate preserved
(dpos exactly ∝ dt at constant input).

## Remaining (needs free lane)

1. Deploy HEAD (c2f7670b+).
2. Knob-proof runs: `MC2_CAMERA_SMOOTH=0` (raw 11.25 spikes in trace) and `=1000`
   (obviously floaty taper) with MC2_CAM_SMOOTH_TEST=1 — compare frames 300-420.
3. FEEL verdict is the user's — rotation is the acceptance test.
