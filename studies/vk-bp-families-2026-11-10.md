# Vulkan best-practices families at nifty 8a39a0c9b (2026-11-10)

Row: box, VVL 1.4.350 (`VK_LAYER_PATH=/workspace/toolchains/vvl-1.4.350.0/explicit_layer.d`), `MC2_VULKAN_VALIDATION=best-practices`,
`capture.py --duration 120` per bookmark (counts scale with frames; before and after use the same duration).
Before: E:/mc2-archive-2026-10/proofs/box/mc2-slot08/8a39a0c9/.tails/row1-25131966.log
After (tip e355c1933): E:/mc2-archive-2026-10/proofs/box/mc2-slot08/e355c193/.tails/row5-25131966.log
All runs: err=0.

## Family table (total counts, 120 s)

| Family | mc2_01 before | mc2_01 after | mc2_10 before | mc2_10 after | Verdict |
|---|---|---|---|---|---|
| Pipeline-SortAndBind | 3672 | 3551 | 6979 | 1609 | real CPU cost; fixed where redundant (below) |
| PipelineBarrier-readToReadBarrier | 43 | 43 | 579 | 582 | not this lane (sync cleanup) |
| AMD-UpdateDescriptors-AvoidCopyingDescriptors | 46 | 46 | 106 | 106 | documented, one-time/streaming copies |
| vkBindBufferMemory/ImageMemory small-dedicated | 28 | 28 | 35 | 35 | load-time, noise |
| SpirvDeprecated_WorkgroupSize | 12 | 12 | 12 | 12 | noise (shader compiler output) |
| AMD-CreatePipelinesLayout-KeepLayoutSmall | 8 | 8 | 8 | 8 | noise |
| AMD-ClearAttachment color / depth-stencil | 6 | 6 | 6 | 6 | noise (a handful of clears) |
| WARNING-Shader-OutputNotConsumed | 3 | 3 | 3 | 3 | known noise, documented not suppressed |
| AMD vkImage StorageRenderTargets / AvoidGeneral | 2 | 2 | 2 | 2 | noise |
| ImageBarrierAccessLayout (s2 proof only, 325) | - | - | not seen in this 120 s row | - | not reproduced; owned by the barrier lane |

## SortAndBind call sites (pipeline name, cmdlabel), counts before -> after

| Site (cmdlabel / pipeline) | mc2_01 | mc2_10 | Action |
|---|---|---|---|
| Present.Hud hudColorTri/hudColorLine/hudTex | 3415 -> 3455 | 281 -> 308 | NOT changed: the HUD draw list alternates triangle/line/textured commands in widget order and blends with alpha, so sorting by pipeline changes the picture. The engine already skips same-pipeline rebinds in that loop. Documented. |
| Shadow.BakeDirty shadow.bake.windowOpaque + alpha | 242 -> 88 | 2484 -> 1018 | removed the redundant opaque rebind at ring entry and exit (the caller had just bound it). Residual: alpha/opaque alternate per tile, depth-only so order is irrelevant, but the tiles are separate records; left |
| Post.HzbBuild posthzb.pipe | 0 | 1771 -> 0 | bind hoisted out of the per-mip render-pass loop |
| Shadow.LocalLive shadow.mechPipe | 0 | 1650 -> 5 | one bind per pass (flag), tiles share it |
| Shadow.Dyn shadow.pipe / mechPipe / alphaPipe | 5 | 777 -> 270 | ring entry/exit rebinds removed; the rest is opaque -> mech -> opaque ping-pong between cascade passes, inherent |
| AA.SMAA1x, Present.Composite, Shadow.BakeObjectMap | ~10 | ~5 | one or two per frame, noise |

Order notes per island: shadow passes are depth-only, so draw order inside a pass does not matter (the ring already splits opaque and alpha). HZB levels are independent render passes. The HUD is order-dependent (alpha blend), so left as is. Not touched, owned by other lanes: terrain, props color, mech, water islands, shadow barrier.
