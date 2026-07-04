# MC2 Vulkan Arc — full piece-by-piece roadmap

**Goal (user):** Vulkan-level ownership over everything. Vulkan as a runtime-selectable render
backend — fully wired, usable, GL fallback. Migrate **piece by piece**: build a parallel Vulkan
renderer and swap pieces onto it (islands = the pieces). **Get to Vulkan, THEN optimize.**

**Method:** every piece = one gated slice, default-OFF byte-identical, proven against the existing
oracle/bookmark harness (`run_golden_parity.py` + `tests/visual/bookmarks/*.json`), adversarial+meta
reviewed when non-trivial, forward-reconned one piece ahead so we never stall on recon. STOP-gate:
any parity/validation failure → gate stays default-OFF forever, piece not shipped.

**Proven foundations (this session):**
- G1 GL↔VK zero-copy interop — PROVEN live (RGBA8 color both directions, validation-clean, 7900 XTX).
- T0 depth-export — VIABLE (D32_SFLOAT sampled bit-exact VK→GL; keep depth dedicated-allocated).
- Reusable primitives shipped: `VkImageResource` (raw-allocated exportable core + double-buffered
  sem/fence) + `GlInteropView` (GL-boundary adapter + LUID match). Self-test PASS in-engine.
- The shipped PostProcess **subgraph** already fuses edge+oob fog as 2 VK draws / 1 render pass /
  1 VK-owned intermediate with NO GL between them — the VK↔VK internal chain already exists; only its
  GL boundaries still round-trip.

**The 4-rung north-star ladder** (each phase climbs one rung across all regions):
```
1. interop SEAM        one shared image crosses GL↔VK zero-copy, one VK consumer
2. VK↔VK CHAIN         ≥2 VK passes share one image, no GL between (parallel-renderer atom)
3. NATIVE SUBGRAPH     a whole screen region rendered entirely in VK, GL only at outer boundary
4. VK-OWNS-PRESENT     VK owns the game window swapchain / final blit — selectable backend + GL fallback
```

---

## PHASE A — Post-process regions onto the zero-copy seam (rung 1→2)

- **P1 — Edge-fog COLOR zero-copy seam** *(IN PROGRESS — T5 proof pending)*. Gate
  `MC2_VK_INTEROP_BRIDGE`. First live proof the seam works in-engine. Oracle: mc2_01 edge bookmarks
  (oblique_fog/oblique_horizon/cliff_side_on/cross_map_view + edge_fog_boundary_south/
  edge_fog_corner_southwest), fog_oob off, byte-identical vs readback island.

- **P1.5 — `VkImageResource` VK→GL semaphore back-channel** *(small, foundational — do BEFORE P2)*.
  Add a second exportable VK→GL semaphore slot (+ import in `GlInteropView`) so the VK→GL hand-off is
  a GPU semaphore wait, not the current per-frame CPU fence. Removes the color-path CPU stall; every
  later piece + the VK↔VK chain wants this. Proof: P1 parity still byte-identical, validation-clean,
  no CPU fence wait on color path.

- **P2 — Fog subgraph (edge+oob) GL boundaries on the seam** *(RECONNED — brief in
  `.claude/VK-NEXT-PIECE-RECON.md`)*. Wrap the already-fused VK↔VK subgraph's INPUT+OUTPUT in
  `VkImageResource`/`GlInteropView` (zero-copy scene-color in, result out). Gate
  `MC2_VULKAN_POSTPROCESS_SUBGRAPH_INTEROP`. First real rung-2 piece. Oracle: edge+oob color vs
  subgraph readback baseline. Depth still readback (P?-depth later).

- **P3 — SSAO island onto the seam** *(recon-first: confirm SSAO I/O + the 5/5 tolerance-floor)*.
  SSAO is parity-proven (5/5 green, 1 within-floor). Put its GL boundaries on the seam. Oracle: the
  existing SSAO backend-compare bookmarks.

- **P5 — Depth zero-copy for the fog/postprocess regions** *(recon `.claude/VK-P5-DEPTH-RECON.md`; BEFORE
  P4; HAS A REAL FORMAT RISK)*. ★CORRECTION: T0 proved **D32_SFLOAT**, but the engine's real
  `sceneDepthTex_` is **GL_DEPTH24_STENCIL8** (gos_postprocess.cpp:933); readback works only because
  glGetTexImage CONVERTS D24S8→f32. Zero-copy aliases bytes — can't convert → P5 needs a **D24S8 /
  D24_UNORM_S8_UINT external import**, UNPROVEN on the 7900 XTX (AMD packed depth-stencil export is
  historically shakier). **MANDATORY P5-T0′ SPIKE FIRST**: extend main_depth.cpp to D24S8; if it FAILS, P5
  (postprocess depth zero-copy) is BLOCKED — do NOT fake with readback; escalate (options: VK-side D32
  depth pre-pass, or accept depth-readback at the GL-produced boundary and only zero-copy depth where the
  PRODUCER is VK). This cascades to P4/HZB depth input. Primitives are otherwise depth-ready (aspect
  handling, dedicated-alloc, GL-internalformat param); depth is INPUT-only (GL→VK, reuses doneSem, no
  vkToGlSem) — simpler than color. Needs a depth-aspect-only sampled view override (D24S8 sampled can't be
  DEPTH|STENCIL). Extend existing per-region gates (depth-on when color-on) + a
  MC2_VK_INTEROP_DEPTH_FORCE_READBACK bisect knob. P5 touches the subgraph TU → REBASE on P2 first; edge-fog
  half is independent (can go first). Tooling checkpoint P5-T0′: parameterize main_depth.cpp +
  vk_interop_transport_test + vkInteropSelfTest to D32 AND D24S8. Oracle: edge bookmarks, depth counters==0,
  validation-clean.

- **P4 — HZB island onto the seam** *(recon done `.claude/VK-P4-HZB-RECON.md`; AFTER P5)*. CORRECTION:
  HZB backend-compare ALREADY PASSED (commit 92b8874c, live region like SSAO) — NOT scaffold, no parity
  prereq. REAL complication = FORMAT: HZB is D32 depth-in + **11× R32_SFLOAT** single-channel out; the seam
  is only proven for RGBA16F. Needs R32F export (greenfield) + D32 import (from P5). **Tooling checkpoint
  T0 (mandatory): parameterize `vk_interop_transport_test` + `vkInteropSelfTest` to R32F, retire the
  single-channel risk offline BEFORE any game launch.** Gate `MC2_VK_HZB_INTEROP`, own device (+6 exts).
  Consumes P5's shared depth as level-0 source + adds 11× R32F shared outputs.

**Phase A exit:** the whole postprocess tail (shadow-consume → SSAO → HZB → fog) reads/writes GL only
at the outer boundary; internal hand-offs are VK↔VK zero-copy.

---

### P5/P4 STATUS (2026-07-03): DEFERRED as optimization
P5-T0′ spike result: `D24_UNORM_S8_UINT` NOT externally-shareable on AMD 7900 XTX
(VK_ERROR_FORMAT_NOT_SUPPORTED); `D32_SFLOAT_S8_UINT` ↔ GL_DEPTH24_STENCIL8 depth-only-view WORKS
(validation-clean, depth sampled exact) — but D32S8 is 64bit/texel vs GL 32bit, so NOT a raw byte-alias
of the existing sceneDepthTex_. Clean P5 requires the shared depth resource be built VK-side with the
PRODUCER rendering depth into it (redirect the scene depth attachment) — invasive (touches all depth
consumers) and it's a CPU-stall OPTIMIZATION, not migration coverage (color already on seam; depth
readback is functionally fine). Per "get to Vulkan THEN optimize": P5 DEFERRED to the optimization phase
(Phase F). P4 (HZB, depends on P5 shared depth) deferred with it. Spike + format recorded for later:
tools/vk_gl_interop_spike/main_d24s8.cpp, P5-T0-d24s8-result.log. Resume P5 when doing depth-stall
optimization; standardize shared depth on D32_SFLOAT_S8_UINT.

### RESEQUENCE (2026-07-03, next-move recon `.claude/VK-NEXT-MOVE-RECON.md`)
Postprocess color seam is proven across 4 regions. Depth (P5/P4) deferred (opt phase). NEXT is
**P13-minimal (backend-selector seam)**, NOT P6. Why:
- **Device-unification blocker:** every island calls vkCreateDevice independently (edge-fog:596,
  oob:469, ssao:673, subgraph:610, hzb:493) + volkLoadDevice into volk's PROCESS-GLOBAL dispatch table.
  No shared device. P6 (chain SSAO→fog) requires building one shared postprocess device, re-parenting 4
  island TUs, AND porting BoxDecals (GL-only, sits between SSAO+fog, gos_postprocess.cpp:1518) →
  highest-cost, least-selectable → P6 is now LAST. ⚠LATENT: global dispatch table = if two islands ever
  live simultaneously, dispatch is whichever loaded last (retro checking).
- **P13-minimal is the smallest step to actually SELECTABLE:** MC2_RENDER_BACKEND read at init; VK path
  creates a swapchain presenting a shared-image BLIT of the GL frame; GL fallback. Keeps GL renderer +
  window intact (GL-compiled-out / VK-owns-renderer deferred to P14). Present machinery already exists
  (VulkanPresentObject: real swapchain/present/resize to a separate window). TOOLING FIRST (P13
  checkpoint): headless swapchain/present + fallback smoke harness.
- New order: **P13-minimal (selector+blit-present) → P8 shadow (smallest geometry) → P9 water →
  P14 VK-owns-renderer → … → P6 chain (last) → P5/P4 depth (opt) → optimize → flip.**

## PHASE B — Grow the native VK subgraph (rung 2→3)

- **P6 — Chain SSAO→fog in VK (no GL between)** *(recon-first: contiguity per Subgraph Contiguity
  Rule — canonical order Shadow→Cloud→Shoreline→SSAO→BoxDecals→EdgeFog→OobFog→Composite; BoxDecals is
  the known contiguity blocker between SSAO and fog — recon whether it can be included or must be
  ported first)*. Extend the fused subgraph to cover more contiguous passes sharing one intermediate.

- **P7 — Native postprocess subgraph covers the full PP tail** *(supervised)*. All contiguous PP
  passes rendered entirely in VK; GL hands scene color in once, gets final PP out once. This is the
  first whole-region-in-VK (rung 3). Oracle: full-PP backend-compare across all bookmarks.

**Phase B exit:** post-processing is a Vulkan-native region. Proven parallel renderer for one region.

---

## PHASE C — Geometry regions (the L7 ladder — big, serial, supervised)

Order = easiest/most-isolated first, terrain/mech last. Each: recon-first → seam → parity vs
bookmarks → (later) native. Binding-table prep (SSBO slot splits) already partly done on L6 lane.

- **P8 — Shadow region live** *(scaffold exists: `vulkan_shadow_island.cpp`, VK-REGION-SHADOW-1 sub-A;
  needs sub-B live backend-compare)*. Depth-only instanced caster in VK; CSM array. Static props
  first, bone-skinned mechs deferred. Oracle: shadow-depth fixtures + live shadow bookmarks.

- **P9 — Water region** *(prep done: WATER-REGISTRY-0 buffers registered, WATER-MVP-ORACLE-1 proved
  column-major MVP + reverse-Z convention)*. MDI water fast-path in VK. Oracle: water bookmarks +
  the MVP oracle.

- **P10 — Terrain region** *(recon-first: biggest; GPU-indirect terrain pipeline, recipe/thin SSBOs,
  cement atlas — coordinate with terrain-indirect expert). Terrain is LAST-but-one for a reason.*

- **P11 — Static-prop region**; **P12 — Mech region** *(mech needs GPU skinning path in VK — heavy)*.

**Phase C exit:** every draw a mission produces can be rendered by the VK backend.

---

## PHASE D — Present / window ownership (rung 4) — the real "selectable backend"

- **P13 — Backend selector at window creation** *(recon-first + supervised; THE architectural seam)*.
  The game window is `SDL_WINDOW_OPENGL` (gos_render.cpp:306), GL-exclusive. Defer window creation
  until `MC2_RENDER_BACKEND` is read; create `SDL_WINDOW_VULKAN` on the VK path. This is what makes
  Vulkan actually *selectable*. GL init compiled-out on the VK path.

- **P14 — VK owns the game-window swapchain / present** *(supervised, XXL)*. Either surface-flip (VK
  presents natively) or final shared-image→GL-backbuffer blit. The L6.6 CompositePresent scaffold
  (separate window) is the starting machinery. HUD/overlay compositing routed (P14b).

- **P15 — GL fallback on VK init failure** *(small; ~90% done — fail-soft pattern exists)*. If VK
  init/interop/LUID fails at launch, force GL for the session, log reason. No crash.

**Phase D exit:** `MC2_RENDER_BACKEND=vulkan` renders a real mission frame and presents it; falls back
to GL if unavailable. **This is the user's stated goal.**

---

## PHASE E — Backend-parity harness + soak

- **P16 — Full-frame backend-compare harness** across all regions + golden scenes.
- **P17 — Soak** the VK path on the campaign missions (tier1 + beyond), validation-clean, no leaks
  (RGD/`VK_EXT_device_fault` for the open sync hazards).

## PHASE F — Optimization (ONLY after parity — user: "then optimize")

- **P18 — Retire remaining CPU stalls / readbacks** (VK→GL semaphores everywhere; the P1.5 pattern
  applied arc-wide).
- **P19 — Bindless** (`VK_EXT_descriptor_indexing`) — kills per-draw rebinds, pairs with MDI +
  GPU-BUFFER-OWNER arc. Highest optimization ROI.
- **P20 — Multithreaded secondary command-buffer recording** (Tracy-measure the prop/unit walk first;
  one pool per-thread-per-frame). The core capability GL can't give.
- **P21 — Dynamic rendering** (`VK_KHR_dynamic_rendering`, not subpasses — RDNA3 immediate-mode),
  transient image aliasing, persistent descriptors, cmd-buffer reuse. RGP-guided.

## PHASE G — Default flip

- **P22 — Flip default to VK** with GL fallback, once parity + soak + measured payoff hold. Shipping
  build gates island TUs appropriately. tier1 smoke 5/5 on the VK path by default.

---

## Standing orders (user, 2026-07-03)
- **Always keep ONE forward-recon queued** so pieces execute back-to-back with no recon stall.
- **Whenever waiting on a background session, SET A FALLBACK WAKEUP** (ScheduleWakeup ~900-1200s) so a
  silently-dead agent is caught in minutes, not an hour of idle. A prior P2 parity agent died silently;
  the run completed but the agent never reported. On wake: check the agent, recover from artifacts or
  re-dispatch (foreground), re-arm if still legitimately running.
- **Build offline harnesses/tools** for parity/proof wherever possible — don't rely on the ~460s
  game-launch golden-parity as the iteration loop. Offline oracle first (seconds), game golden-parity
  as FINAL confirmation only. First tool: headless interop-transport parity harness (see TOOLS below).
- **Light adversarial retro every 5-6 slices** — re-attack PAST landed slices (did they actually hold?
  drift? regression? foundation still sound?). SLICE COUNTER: P1 done. Next retro due ~after P5/P6.
- **TOOLING-REFRESH CHECKPOINT** — at the START of every NEW *type* of piece (first PP-boundary, first
  depth, first VK↔VK chain, first geometry region, first present piece, ...) AND every ~3 slices: pause
  and ask "what offline harness / proof tool / oracle would make THIS type fast + provable, and does it
  exist yet?" If not, build the tool BEFORE grinding the slices manually. Update the Tooling Ledger below.
  A new type usually needs a new offline oracle — build it once, reuse it across every piece of that type.

## Tooling ledger (what exists / what each new TYPE needs — refresh at each checkpoint)
| Tool | Covers | Status |
|---|---|---|
| `tools/vk_gl_interop_spike` (color) | G1 zero-copy color transport primitive | ✅ built, PASS |
| `tools/vk_gl_interop_spike` (main_depth) | T0 D32 depth transport primitive | ✅ built, VIABLE |
| `tools/vk_interop_transport_test` | offline per-piece transport-transparency parity (seconds) | ✅ built, PASS (neg-test verified) |
| forbidden-counter asserts (in-island) | "no readback on interop path" proof | ✅ pattern established |
| golden-parity game launch | final real-pixel confirmation (~460s) | ✅ existing, keep as FINAL only |
| *(new type: VK↔VK chain)* | offline 2-pass-shared-image parity | ❌ build at P6 checkpoint |
| *(new type: geometry region)* | headless geometry-region parity (props/terrain draw) | ❌ build at P8 checkpoint |
| *(new type: present/window)* | headless swapchain/present + fallback smoke | ❌ build at P13 checkpoint |

## TOOLS / offline harnesses (build these to speed + de-risk the proof loop)
- **interop-transport parity harness** (tools/, headless, seconds) — for a given VK pass + fixed golden
  inputs, render it BOTH ways (readback-bridged image vs interop-shared VkImageResource) and assert
  output byte-identical + forbidden-counters==0 + validation-clean. Parameterizable per island. Proves
  the transport is transparent for that shader WITHOUT a game launch. Run per-piece before the full
  golden-parity. (IN FLIGHT.)
- Reuse/extend `tools/vk_gl_interop_spike` (G1 color + T0 depth) as the primitive substrate.

## Standing rules for every piece
- Default-OFF byte-identical (cmp the gate-OFF exe); register+allowlist+document the gate.
- Prove with the oracle bookmarks, not new fixtures; forbidden-counter hard-asserts for "no readback";
  `MC2_VULKAN_VALIDATION=sync` clean; direct `cmp` on harness-masked bookmarks.
- Forward-recon the NEXT piece while the current one builds. Adversarial+meta review non-trivial plans.
- STOP-gate: any failure → default-OFF forever, diagnose, don't paper over.
- RelWithDebInfo island variant (`build64_island`, `-DMC2_VULKAN_ISLAND=ON`); full relink on layout
  changes; deploy via `deploy_payload.py --build-dir build64_island`.
- Honest framing: "seam/region proven in VK", never oversell as "backend flippable" until Phase D.

## Known open items carried in
- CMake: `vulkan_shadow_island.cpp` unregistered on L6 lane (fixed in this worktree; chip filed).
- `check-env-registry.sh` pre-existing RED for older VK-island gates (not ours).
- fog_oob is AMD-GLSL vs glslang noise-divergent (S5/S6) — excluded from oracles, not a bug to fix here.
- SSBO:2 terrain-mask-solid collision + island readback R-A-W hazard — ledgered VK-prep TODOs.
