# VULKAN ARC — STRATEGY, GO/NO-GO GATES, AND EASY WINS (2026-07-02)

Decision-grade strategic record for the MC2 Vulkan-backend arc. Synthesized from a parallel
adversarial/meta review + full-vulkan-path + optimization/RT/upscaling deep-dives + two
web-research recons (D3D-via-Vulkan, GitHub easy-wins) + the empirical shadow-consume result.
Target: fixed single AMD Radeon RX 7900 XTX (RDNA3, Windows AMD Vulkan driver). Companion to
`VULKAN-REMAINING-LADDER-EXECUTION-PLAN.md` (the HOW) — this is the WHY / WHETHER.

## The core finding
Every ladder doc proves each *step* is safe and is silent on whether the *destination is worth
reaching*. Two make-or-break unknowns sit at the FINISH LINE behind ~15 region ports:
1. **Shared-image interop (`GL_EXT_memory_object` / `VK_KHR_external_memory_win32`) — 0% built,
   UNPROVEN on this GL+VK driver combo.** If it fails, every region port to date was throwaway
   oracle code by construction.
2. **Is Vulkan even faster?** The ONLY perf datapoint says it's ~25% SLOWER (readback bridge
   59 vs 79 fps). EMPIRICALLY REINFORCED 2026-07-02: the shadow-consume readback at 8192² =
   256MB×2/frame = **~316 ms/frame (~3 FPS)** — the CPU-readback oracle is catastrophically
   unusable for large resources. The remaster is almost certainly **GPU-bound** (PBR/CSM/HDRI/
   water ~10ms), not CPU-bound — and CPU draw-submission is the ONLY axis Vulkan's core win
   attacks. Parity discipline also means the VK path can only ever TIE GL pixels, so the
   default-flip is justified ONLY by unmeasured perf.

## GO/NO-GO GATES (the decision record the arc was missing)
- **G1 (run NEXT, before any more region ports):** a standalone interop spike — export a VkImage
  (R16F/D32/RGBA16F) via VK_KHR_external_memory_win32, import into GL via GL_EXT_memory_object,
  sample + read back byte-identical BOTH directions, validation-clean, zero CPU copy. **NO-GO =
  full-frame Vulkan is unreachable on this HW → land at HYBRID** (GL owns frame+window, VK
  regions offscreen as a parity/portability asset). Do NOT rescue with a greenfield rewrite.
- **G2 (parallel with G1):** RGP/RenderDoc-capture the current GL frame on tier1 → write down
  where CPU *and* GPU time actually go. If GPU-bound, the core Vulkan rationale is gone.
- **G3 (write it):** the missing half-page — "why Vulkan vs stay-GL-4.3 vs Zink vs greenfield" +
  a measured user-visible payoff criterion ("VK must deliver X ms/frame on mc2_24 or we don't
  flip"). Vulkan buys FEATURES (RT, FSR4-INT8, bindless perf), not necessarily perf — if G1/G2
  say "works but not faster", scope a MINIMAL VK interop for exactly those features.

## Full-vulkan critical path (serial, immovable)
interop-probe → retire-readbacks → fuse-one-cmd-buffer → barrier-DAG → VK-own-present →
default-flip. Two gates: interop (#1) + game-window present-ownership surgery (#2). Honest
total: 8–14 months solo, L7 geometry + fusion dominate, either gate can end it early.

## Optimization frontier (once on Vulkan, ranked ROI)
1. **Bindless / `VK_EXT_descriptor_indexing`** (highest — kills per-draw rebinds, pairs with MDI
   + GPU-BUFFER-OWNER arc). 2. **Multithreaded secondary-cmd-buffer recording** (Tracy-measure
   the prop/unit walk first; one pool per-thread-per-frame). 3. GPU-driven indirect deepening.
   4. Readback retirement (`VK_KHR_dynamic_rendering`, NOT subpasses — RDNA3 is immediate-mode
   desktop). SKIP mesh shaders + async compute until measured GPU-bound. ~90% of the win =
   bindless + MT-submission + GPU-driven.

## Ray tracing (capstone, AFTER geometry-native-in-VK)
RDNA3 is fine on SPARSE RT. Best fits (top-down/oblique camera): ① RT contact/soft shadows
(retires CSM stutter/seam/bias) ② RT water reflections (SSR can't do off-screen on oblique
water) ③ RTAO. SKIP RT-GI/path-tracing (RDNA3 weakness). Use `VK_KHR_ray_query` (inline,
composes with the parity model), NOT RT-pipeline/SBT. First feature: `MC2_RT_SHADOWS` default-OFF
A/B vs CSM. No DLSS Ray Reconstruction on AMD → own the denoiser.

## Upscaling
DLSS is OUT (NVIDIA Tensor lock). FSR is the answer (FSR 3.1 unified API now → FSR 4.1 INT8 later
on the 7900 XTX). ★ **FSR 2/3.1 can ship on the CURRENT GL path NOW** (post-process compute,
~1200-line proven OpenGL port, slots into gos_postprocess.cpp). Prerequisite = motion vectors
(engine produces NONE: needs Halton jitter + prevViewProj + a per-unit previous-transform lane).
SKIP FSR frame-gen (input latency — wrong for an RTS). Honest: CPU-bound → upscaling is a quality
knob, not an fps win.

## EASY WINS — do now, on the CURRENT GL path (decoupled from the Vulkan arc)
| Rank | Integration | License | Cost | ROI |
|---|---|---|---|---|
| 1 | **FidelityFX CAS** (sharpen post-pass → gos_postprocess.cpp) | MIT | Trivial | HIGH — cheapest visible win |
| 2 | **FidelityFX SPD** (single-pass mip/HZB reduce, replaces our HZB reduce) | MIT | Small | HIGH (pairs w/ GPU-cull) |
| 3 | **meshoptimizer** (vertex-cache + LOD for Assimp-imported mechs) | MIT | Trivial | MED-HIGH, API-agnostic |
| 4 | **RenderDoc in-app API** (scripted MC2_*-gated capture) | free | Trivial | MED-HIGH (workflow) |
Queue behind Vulkan: AMD **RGP** markers (Vulkan-only, no GL) + **RGD**/`VK_EXT_device_fault`
crash dumps (for the open sync hazards). DROP: Zink (driver not lib, no fine control — not a
shortcut), nvrhi (no GL backend), XeSS (worse than FSR on AMD), FSR frame-gen (RTS latency).

## D3D-via-Vulkan (DXVK/vkd3d) — borrow patterns, NOT a shortcut
Remaster is GL not D3D → translation layers don't apply at runtime; native is the only RT-capable
route. But BORROW: **DXVK (zlib — code-borrowable w/ attribution): its memory allocator (PR #4280
— global size-ordered free list, 64KiB granularity, best-fit, power-of-two pool) directly serves
GPU-BUFFER-OWNER-RECON-1**; its command-pool/list role-split + submission queue = FRAME-JOBS
blueprint. **vkd3d-proton (LGPL — lessons only): barrier batching (`vkCmdPipelineBarrier2`, aim at
the MDI/indirect path) + `VK_EXT_descriptor_buffer` bindless.**

## Ecosystem: upstream = alariq/mc2 (cherry-pick bugfixes); Alexbeav Restoration-Project (FMV/
FFmpeg + installer reuse). Ms-LPL license on the MS source drop.

## RECOMMENDED NEXT 3 MOVES
1. **Interop spike (G1) + GL perf baseline (G2) THIS WEEK** — before any more region ports.
2. **Ship FSR 2/3.1 + CAS on the CURRENT GL path** — highest-ROI user-visible win, zero Vulkan
   dependency, builds the MV/jitter infra RT later reuses. Slices: MC2_TAA_JITTER → terrain
   reprojection MV → MC2_RENDER_SCALE → MC2_FSR / MC2_CAS.
3. **Write the G1/G3 decision record + kill/rescope criteria** into the ladder, then branch on
   G1's result.
Through-line: de-risk the two unknowns now; ship the user-visible GL wins regardless; refuse to
climb the remaining mountain until there's a written, MEASURED reason the summit is worth reaching.
