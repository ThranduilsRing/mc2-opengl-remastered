# tools/gfxr: GFXReconstruct capture / replay (box)

Pinned toolchain on the box: `/workspace/toolchains/gfxr-1.4.350.0` (LunarG Vulkan SDK 1.4.350.0, GFXReconstruct 1.0.5).
The PC SDK (`C:/VulkanSDK/1.4.350.0`) carries the same version.

```
py -3 tools/gfxr/capture_replay.py capture --tag base --mission mc2_10 --bookmarks mc2_10_camera --pose wolfman
py -3 tools/gfxr/capture_replay.py replay  --tag base -n 7
py -3 tools/gfxr/capture_replay.py ab --a base --b cand -n 7
```

- `capture`: box_run `sh:` row; runs `scripts/run_visual_capture.py` (never mc2 directly) with the GFXR layer env set
  (`VK_LAYER_PATH`, `VK_INSTANCE_LAYERS`, `GFXRECON_CAPTURE_FILE/FRAMES`). The `.gfxr` stays on the box in `/workspace/gfxr/<tag>/`.
  `--env KEY=VAL` passes extra layer options (for example `GFXRECON_MEMORY_TRACKING_MODE=unassisted`).
- `replay`: `gfxrecon-replay --measurement-file --flush-measurement-range` N times; prints median and IQR of ms/frame.
- `ab`: two captures from two builds at the same pose, replayed interleaved N times each. Replaying ONE capture on two builds is not an A/B.

## Status

Written and committed, NOT yet proven end to end. First live capture attempt (page-guard default) ended in `device lost`
before the capture window under the layer (mc2 ran at about 1 fps with the layer, a 5 s fence wait expired). The
`--env GFXRECON_MEMORY_TRACKING_MODE=unassisted` retry never ran: box GPU 0 stayed busy for 30 min. Next step is that retry.

## Caveat

Replay timing is a guide only (box RTX 3060, ratio-only). RGP on the PC 7900 XTX stays the proof for any GPU-time claim.
