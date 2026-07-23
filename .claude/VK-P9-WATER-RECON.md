# VK-P9 WATER RECON — first GEOMETRY region (GL produces inputs, VK renders water, GL composites)

Worktree `A:/Games/mc2-vk-interop-bridge`, branch `claude/vk-interop-bridge-1`, HEAD `19cf4fb8`.
Prep origin: WATER-REGISTRY-0 `83da3973`, WATER-MVP-ORACLE-1 `4d30d93e` (merged `2f4aafcc`).
Compare doc: `.claude/VK-P8-SHADOW-RECON.md`. Roadmap: `.claude/VK-ARC-ROADMAP.md` (P9, Phase C).

---

## VERDICT

**Water IS genuinely the easier first geometry region — but it is NOT trivially small, and its
easiness comes from a DIFFERENT source than P8's would.** The two hardest things about a first
geometry region are (1) the SEAM DIRECTION and FORMAT, and (2) the GEOMETRY FEED. Water wins
decisively on BOTH versus shadow:

- **Direction + format: WATER = the postprocess seam, unchanged.** Water's output is an
  **RGBA16F color contribution** composited into MainColor — the exact proven seam
  (SSAO/fog/HZB-color all RGBA16F, GL-consumes). **No VK-produces-depth direction flip, no vk2gl
  back-channel to light up, no D24S8 blocker, no `sampler2DShadow` compare-mode-over-imported
  unknown.** Shadow was the mirror-image on BOTH axes at once (VK PRODUCES + geometry feed); water
  keeps the seam identical to what is already proven 4×.
- **Geometry feed: ALREADY GPU-RESIDENT (WATER-REGISTRY-0).** The water mesh is not marshaled
  per-frame from CPU like shadow casters. It is an MDI fast-path whose 4 buffers
  (thin/recipe/header/indirect-cmd) are **live, GPU-resident, and now registered with
  GpuBufferOwner** so a VK resolver can bind them directly. Shadow's biggest cost (R1: build a
  per-frame CPU→GPU caster-instance marshaling path with no precedent) **does not exist for water.**
- **MVP convention: ALREADY SETTLED (WATER-MVP-ORACLE-1), transpose-bug pre-dodged.** The single
  highest-risk trap (the LM1/N0 transpose trap that bit SSAO-2b and fog-N0) is retired offline,
  BIT-EXACT, against an asymmetric/sheared matrix.

**BUT — water is not a pure screen-space pass; it draws a real mesh AND has an input dependency**
(HDRI + a GL-produced terrain-reflection RT + tex1/tex2 water textures + depth-test against scene
depth). That makes it a genuine geometry region (mid-body compute dispatch + MDI draw), not a
full-screen shader. The lift is: stand up a VK graphics pipeline that binds the already-resident
MDI buffers, renders the water mesh into a VK-owned RGBA16F attachment, and composites it back —
GL feeds the input textures in, GL gets the water-blended color out. **This is the SSAO island
shape (GL→VK color in, VK renders, RGBA16F out) plus a mesh draw instead of a full-screen quad.**

**Smallest first water sub-slice (P9a): recon confirms — see §7.** Prove the MDI-buffer bind +
RGBA16F color seam with reflection/HDRI DISABLED (their gates default in a way that lets water be a
plain textured+depth-tested overlay), before wiring the reflection-RT input.

**Is water genuinely easier than shadow? YES — recommend water goes first.** Shadow's advantage was
only "the pipeline scaffold exists." Water's advantages (proven seam direction+format, GPU-resident
geometry feed, settled MVP, no back-channel, no format blocker, no compare-mode unknown) outweigh
that. Shadow should follow water, reusing whatever pipeline plumbing water establishes.

---

## 1. HOW WATER RENDERS TODAY

**Entry / call site:** `land->renderWaterFastPath()` — `code/gamecam.cpp:552`, editor
`editor/EditorCamera.h:334`, gpu-driven camera `code/simplecamera.cpp:261`. Thunk
`gameos_graphics.cpp:3169`; body `gosRenderer::renderWaterFastPath()` `gameos_graphics.cpp:3181`.
It runs INSIDE renderWater, AFTER renderLists() has drained (Object/HUD passes done) —
`simplecamera.cpp:255-261`. Self-guards to no-op unless the GPU-driven path is armed.

**It is a GEOMETRY pass, not screen-space.** It draws the water-surface **mesh** via
`glMultiDrawArraysIndirect` (the MDI fast path). Evidence: the VS
`shaders/gos_terrain_water_fast_mdi.vert` transforms real `worldPos` through the MVP and emits
`gl_Position` (:283-286: `clip = u_worldToClipGL * vec4(worldPos,1); clip.z += WATER_DEPTH_FUDGE_FAST*clip.w`),
indexed by `gl_DrawIDARB` into per-command SSBOs. This is vertex geometry, not a full-screen quad.

**Pass structure (self-contained, clear boundaries):**
- **Mid-body COMPUTE dispatch** — `WaterStream::ComputeDispatchAndBindThinRecords(frameCos)`
  (`gameos_graphics.cpp:3362`) cull-packs thin records + binds them to SSBO slot 6. This is the
  "mid-body COMPUTE dispatch" the file header warns about (`gameos_graphics.cpp:2215`).
- **MDI draw** — lazy-compiled `gos_terrain_water_mdi` program (`:3232`), `glMultiDrawArraysIndirect`
  over the registered indirect-cmd buffer (`mdiValid` gate `:3466`).
- **State:** alpha-blend overlay (SRC_ALPHA/ONE_MINUS_SRC_ALPHA), depth-TEST ON reverse-Z GEQUAL,
  depth-WRITE ON (OOB-FOG-WATER-DEPTH-1, `:3432`), cull None. Driven by the WaterArmed pipeline desc
  (`:3417-3419`). REPEAT/LINEAR sampler on unit 0 (`:3443-3458`).

**Input (the important part — water HAS inputs):**
| Input | Unit | Source | Format | Default |
|---|---|---|---|---|
| Water base tex (`tex1`) | 0 | `gos_terrain_bridge_glTextureForGosHandle(waterGosHandle)` `:3460` | GL tex | always |
| Water detail (`tex2`) | 1 | detail gos handle `:3461` | GL tex | when present |
| Terrain reflection RT (`u_waterReflRT`) | 2 | `getGosPostProcess()->getWaterReflectionTexture()` `:3616` | **GL_RGBA16F** (`gos_postprocess.cpp:1115`) | gate `MC2_WATER_REFLECTION_RT` **OFF** by default (`:3126`) → FS skips RT, SH-sky fallback |
| HDRI equirect (`u_hdri`) | 3 | HDRI, mipmapped `:3624` | GL tex | reflection-strength gated |
| **Scene depth** | — | depth-TEST vs bound depth buffer | depth | always (occlusion at shore) |

**Output:** the water color is alpha-blended directly into the frame's **MainColor** attachment
(RGBA16F scene color). Water HAS an fbo_ledger MainColor row + an AmbientContract row now
(`gameos_graphics.cpp:3217-3220`). So the output boundary = MainColor RGBA16F — the proven seam.

## 2. SEAM FIT — can water's boundaries go on the interop seam like SSAO?

**YES for the OUTPUT; the INPUTS are all GL textures fed in (same as SSAO's scene-color-in).**

The SSAO island template (`vulkan_ssao_island.cpp`) is: GL scene-color/normal/depth in →
VK renders into an exportable **RGBA16F** `VkImageResource` that IS the pass attachment → GL
consumes the shared texture (readback path retired, zero-copy). SSAO's format constant
`kOcclusionFmt = VK_FORMAT_R16_SFLOAT` (`ssao_island.cpp:357`) proves single-channel; the color
output uses `glInteropViewCreate(..., GL_RGBA16F)` (`ssao_island.cpp:707`) — **exactly water's
output format.** Water maps onto this cleanly:
- **VK output = one exportable RGBA16F `VkImageResource`** (the water pass color attachment),
  imported into GL via `GlInteropView`/`GL_RGBA16F`. GL then alpha-composites it into MainColor
  (or the water pass renders into a MainColor-aliased shared image — decide in P9a; simplest first
  is: VK renders water-over-transparent into its own RGBA16F, GL blends it onto MainColor, matching
  the existing alpha-blend math).
- **Inputs stay GL-fed** (SSAO precedent): water base/detail textures, HDRI, and — when its gate is
  on — the reflection RT are GL textures made available to the VK pass. For P9a keep the
  reflection-RT gate OFF (its default) so the only inputs are the two water textures + depth-test.

**Does water need the geometry-feed machinery that made shadow a big lift? NO.** Shadow needed a
brand-new per-frame CPU→GPU caster-instance marshaling path (P8 R1, "where the slice cost lives").
Water's geometry is the MDI fast path: **the buffers are already GPU-resident and now registered**
(§3). VK binds them directly as SSBOs + the indirect-cmd buffer; no per-frame CPU marshaling. The
one wrinkle: the mid-body COMPUTE cull/pack (`ComputeDispatchAndBindThinRecords`) currently runs on
GL. For P9a either (a) keep the GL compute producing the thin records and hand them to VK
(the buffers are shared/registered), or (b) port the compute too. **(a) is the smaller first step**
— the seam only needs to cover the graphics draw; the compute stays a GL producer feeding a shared
SSBO, exactly the "GL produces inputs" shape.

## 3. WHAT WATER-REGISTRY-0 / WATER-MVP-ORACLE-1 ALREADY GIVE

**WATER-REGISTRY-0 (`83da3973`) — the geometry feed is already GPU-resident + resolver-mappable.**
The 4 live MDI water buffers were file-static globals never registered; the commit pairs each with a
GpuBufferOwner identity + registers them in RenderResourceRegistry (create) / invalidates (destroy),
byte-identical (raw GLuint globals unchanged, metadata only):
```
g_thinBuffer             -> WaterThinBuf        (id 43, fenced ring-3)   binding 6
g_recipeBuffer           -> WaterRecipeBuf      (id 44)                  binding 5
g_waterBucketHeaderSsbo  -> WaterHeaderBuf      (id 45)
g_waterIndirectCmdBuffer -> WaterIndirectCmdBuf (id 46, MDI cmd source)
```
`scripts/check-gpu-buffer-owners.py` PASS (4 water buffers OWNED, Mission lifetime). **Meaning: a VK
water region's resolver can map these directly — no per-frame marshaling, no geometry-feed lift.**
This is the single biggest reason water beats shadow as first geometry.

**WATER-MVP-ORACLE-1 (`4d30d93e`) — the MVP convention is SETTLED and transpose-bug-proofed.**
Headless clip-space oracle (build64_vulkan probe, `shaders/vulkan/water_mvp.{vert,frag}`) proves the
VK water UBO pack reproduces the GL VS BIT-EXACT before any real port. VERDICT it hands the port:
**pack the SAME raw 16 floats GL uploads (`glUniformMatrix4fv(loc,1,GL_FALSE,wMvp)`,
`gameos_graphics.cpp:3310/3532`) into a `column_major` (std140 default) mat4 — do NOT `row_major`-
qualify, do NOT transpose — and compute `M*worldPos`.** Same rule as edge_fog.frag / ssao.frag (N0).
Reverse-Z fudge lockstepped: `WATER_DEPTH_FUDGE_FAST=0.00025`, applied `clip.z += fudge*clip.w`
pre-divide (`mclib/terrain_depth_bias.h:77` ↔ `.hglsl:65`). The WRONG (transposed) pack diverges by
~69.5 — a real micro-check, not tautological. **This dodges the highest-risk L7.4 trap up front.**

**Net: the two hardest new-machinery risks (geometry feed + MVP transpose) are BOTH already retired.**
What P9 must build is the VK graphics pipeline + RGBA16F color seam + compositing — the SSAO shape.

## 4. FORMAT + DIRECTION

- **Output = RGBA16F color, GL-CONSUMES.** Confirmed: water composites into MainColor (RGBA16F),
  and the reflection RT it reads is `GL_RGBA16F` (`gos_postprocess.cpp:1115`). Same as SSAO
  (`glInteropViewCreate GL_RGBA16F`, `ssao_island.cpp:707`). **No D24S8/depth-produce problem** —
  the P5 blocker (D24_UNORM_S8 not externally-shareable on 7900 XTX, roadmap:85-94) is entirely
  irrelevant; water never produces depth across the seam. **No vk2gl back-channel needed** (that was
  shadow's R4) — this is the GL-produces-inputs / VK-consumes-and-renders / GL-consumes-output
  direction already proven; GL is the pure producer of inputs, VK the pure producer of the color
  result exactly like SSAO.
- **Direction has NO flip.** Postprocess = GL produces → VK consumes → result to GL. Water = same.
  Shadow's defining characteristic (VK PRODUCES depth → GL consumes) does not apply.
- **INPUT dependency to note (parity + wiring):** the reflection RT (`u_waterReflRT`, RGBA16F,
  `gos_postprocess.cpp:1115`) is filled by a SEPARATE GL pass — `RenderWaterReflectionPass()`
  (`gos_terrain_indirect.cpp:3861`, called `gamecam.cpp:544` BEFORE water). It is **gated OFF by
  default** (`MC2_WATER_REFLECTION_RT`, `gameos_graphics.cpp:3126`; `u_waterRtStrength=0` → FS skips
  the RT sample → SH-sky fallback, byte-identical to Phase A). **P9a can therefore ignore the
  reflection input entirely** and prove the color seam with just the two water textures + depth-test.
  Wiring the reflection RT as a second GL-produced input (RGBA16F, same seam) is a later sub-slice
  (P9b), and it's straightforward because the RT is a normal GL texture, not VK-produced. HDRI
  (`u_hdri`) is similarly a bind-in GL texture, defaultable off via reflection strength.
- **Depth INPUT:** water depth-TESTS (GEQUAL reverse-Z) against the scene depth buffer so the shore
  occludes correctly. The VK water pass needs read access to scene depth as a **depth attachment for
  testing** (not sampled), OR it renders water into its own RGBA16F+depth and GL composites with the
  original depth. For P9a the cleanest is: GL supplies scene depth to the VK pass as the depth
  attachment (depth-test only, no depth-produce-across-seam), OR do the depth-test/composite on the
  GL side after importing the VK water color. Note this is the one place a depth INPUT appears — but
  it's GL→VK (import), the simpler direction, and does NOT hit the D24S8 export blocker.

## 5. DEVICE

Reuse the shared `VulkanContext` via `getOrCreateEngineVulkanContext()`
(`vulkan_context.h:111`) — the header explicitly names **P9** as a reuse client
(`vulkan_context.h:8-9`: "P6/P8/P9/P14 all reuse the SAME device"). Borrow
`instance()/physicalDevice()/device()/graphicsQueue()/graphicsFamily()` (`vulkan_context.h:80-83`)
exactly as `EnginePresentVk::init()` does (`engine_present_vk.cpp:330-337`); never a second
`volkLoadDevice` (the roadmap's latent global-dispatch-table hazard). **R2 interlock:** the water
pass's exportable RGBA16F `VkImageResource` needs the external-memory/-semaphore Win32 extensions,
so P9 must assert `wantInterop` was set at context birth (`vulkan_context.h:63-64,71`,
`init(bool wantInterop,...)`). The shared device created only the gfx queue family (one queue);
water records its graphics + (if ported) compute on that same queue — fine, no present needed for
the water produce (present ownership is P13/P14, already partly live at HEAD `19cf4fb8`).

## 6. ORACLE

- **Offline transport harness (build FIRST):** reuse the RGBA16F transport pattern —
  `tools/vk_gl_interop_spike/main.cpp` (RGBA8) / the SSAO island's readback-vs-interop A/B — to prove
  the water RGBA16F seam byte-identical (readback-bridged vs shared VkImageResource) with
  forbidden-copy-counters==0 (`ssao_island.cpp:139` `vkCmdCopyImageToBuffer==0` on interop) and
  validation-clean. RGBA16F transport is ALREADY the proven format (SSAO), so the transport risk is
  effectively pre-retired; the new offline check is "the MDI water buffers bind + the VK VS produces
  the same clip pos" — and **WATER-MVP-ORACLE-1 (`shaders/vulkan/water_mvp.{vert,frag}`) already IS
  that oracle**, BIT-EXACT. Extend it minimally to render the actual water mesh into the shared
  RGBA16F rather than a fixture readback if a mesh-level offline check is wanted.
- **Live golden-parity:** water bookmarks exist — `tests/visual/bookmarks/mc2_01_water.json`
  (plus `mc2_01.json`, `mc2_24.json`) via `run_golden_parity.py`. Oracle = "VK-rendered water,
  GL-composited, yields byte-identical (or within noise-floor) final pixels vs GL-rendered water."
- **Known PARITY RISK — water z-fight / depth-fudge sensitivity (mc2-water-depth-expert domain).**
  Water is razor-thin on depth: `WATER_DEPTH_FUDGE_FAST=0.00025` pre-divide, reverse-Z GEQUAL, and
  "water jumps/recedes on zoom" is a documented sensitivity. The VK pass MUST reproduce the fudge
  bit-for-bit (WATER-MVP-ORACLE-1 already lockstepped it) AND the same reverse-Z GEQUAL depth-test
  and depth-WRITE-ON (OOB-FOG-WATER-DEPTH-1). Any drift → shoreline shimmer / OOB-fog-over-water
  regression. This is the parity analog of the framegraph depth-tie failure; treat depth state as a
  hard bit-exact contract, not "close enough."

## 7. TASK BREAKDOWN + RISKS + BIGGEST UNKNOWN

**P9a (recommended first slice — plain textured+depth-tested water into RGBA16F, reflection OFF):**
1. **Tooling first:** RGBA16F water transport parity harness (readback vs shared VkImageResource,
   forbidden-counters==0, validation-clean). Leverage WATER-MVP-ORACLE-1 for the VS clip-pos proof.
2. Re-parent onto the shared `VulkanContext` (`getOrCreateEngineVulkanContext`, assert `wantInterop`);
   never a second `volkLoadDevice`.
3. Build a VK graphics pipeline for the water MDI draw: bind the 4 registered MDI buffers
   (WaterThin/Recipe/Header/IndirectCmd, bindings 5/6/7 + indirect source) directly (WATER-REGISTRY-0
   makes them resolver-mappable), water base/detail textures fed in from GL, WaterArmed state
   (alpha-blend, reverse-Z GEQUAL, depth-write on, cull none), MVP packed per WATER-MVP-ORACLE-1
   (column_major, GL_FALSE raw bytes, +fudge).
4. Render water into an exportable RGBA16F `VkImageResource` (SSAO template) + `GlInteropView`
   (`GL_RGBA16F`); GL alpha-composites onto MainColor (or VK renders into a MainColor-aliased shared
   image). Keep the GL compute cull/pack as a producer feeding the shared thin SSBO (don't port
   compute yet).
5. Reflection RT + HDRI OFF (their defaults) so inputs = base/detail textures + scene-depth test only.
6. Gate `MC2_VK_WATER_INTEROP` (register + allowlist + document), default-OFF, gate-OFF exe
   byte-identical.
7. Prove: offline harness green → `mc2_01_water` / `mc2_24` golden-parity byte-identical (or noise
   floor) → forbidden-counters==0 → `MC2_VULKAN_VALIDATION=sync` clean.

**P9b (later):** wire the GL-produced reflection RT (RGBA16F) + HDRI as VK pass inputs; enable
`MC2_WATER_REFLECTION_RT` parity. **P9c:** port the water compute cull/pack (`WaterStream::
ComputeDispatchAndBindThinRecords`) into VK so the whole region (compute→draw) is VK-native (rung-3,
couples to P7-style native subgraph work).

**Risks:**
- **R1 (biggest — parity, not machinery): depth-fudge / z-fight bit-exactness.** Reverse-Z GEQUAL +
  0.00025 pre-divide fudge + depth-write-on must match GL exactly, or shoreline shimmer / OOB-fog-
  over-water. Mitigated up front by WATER-MVP-ORACLE-1's lockstepped fudge; verify on the
  zoom-sensitive water bookmarks. (mc2-water-depth-expert domain.)
- **R2: the mid-body compute dispatch.** Water is compute-then-draw. Keeping GL compute as a producer
  (P9a) sidesteps porting it; but the shared thin SSBO (ring-3 fenced, id 43) must be correctly
  synchronized GL-writes → VK-reads. Use the SSAO island's readback-sync barrier discipline
  (VULKAN-ISLAND-READBACK-SYNC-BARRIER-1) adapted to a GL→VK buffer hand-off.
- **R3: compositing math.** Alpha-blend SRC_ALPHA/ONE_MINUS_SRC_ALPHA onto MainColor must be
  reproduced identically whether the blend happens VK-side (into a MainColor-aliased shared image) or
  GL-side (import water color, blend on GL). Pick one and pin it; GL-side blend is lower-risk for P9a.
- **R4: depth INPUT wiring.** Water depth-tests against scene depth. Feeding GL scene depth to the VK
  pass as a depth attachment for TEST-only is GL→VK import (simpler direction, no D24S8 export
  blocker) — but the engine scene depth is GL_DEPTH24_STENCIL8 (P5 note); for TEST-only VK may need a
  depth attachment it renders/tests against without cross-seam depth sharing. Simplest P9a: composite
  + depth-test on the GL side after importing the VK water color, avoiding any depth across the seam.
- **R5: SSBO binding collision.** The roadmap flags an open "SSBO:2 terrain-mask-solid collision" and
  binding-table prep on L6. Water uses bindings 5/6/7 — confirm no collision with the shared device's
  descriptor layout.

**Biggest unknown:** **the compute→SSBO→draw GL/VK hand-off synchronization (R2)** — not the seam
(proven), not the geometry feed (registered), not the MVP (oracle-proven), but whether GL's per-frame
compute-packed thin records are correctly visible to a VK draw reading the same shared fenced ring
buffer without a hazard. Resolve it in the P9a offline harness: run the GL compute, hand the shared
thin SSBO to the VK draw, assert the VK-rendered water color matches the all-GL path bit-for-bit.
If GL→VK buffer visibility proves fragile, fall back to P9a rendering from a VK-side re-pack of the
recipe buffer (mission-static, no per-frame race) — a known workaround, not a blocker.
