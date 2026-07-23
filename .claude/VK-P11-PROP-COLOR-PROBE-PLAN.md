# VK-P11 STATIC-PROP COLOR — Tier-0 seam probe plan

**Worktree:** `A:/Games/mc2-vk-interop-bridge` · branch `claude/vk-interop-bridge-1` @ `ecbdf98c`
**Gate:** `MC2_VK_PROP_COLOR_INTEROP` (default-OFF, byte-identical gate-OFF exe)
**Goal:** First VK-native *color geometry region*. Prove VK can render real static-prop
geometry into a shared exportable RGBA16F image that GL composites — the keystone the
terrain (P10) and mech (P12) color regions both inherit.

## Scope discipline — TIER 0 (this probe) vs TIER 1 (follow-up)
- **TIER 0 (THIS probe, seam proof — NOT full parity):** VK renders the static-prop color
  pass into a shared RGBA16F with its OWN VK-owned D32 depth (cleared each frame → inter-prop
  occlusion correct). GL alpha-composites the VK result over the scene at the prop-draw point.
  Prop-vs-terrain occlusion is APPROXIMATE (props composite over terrain) — acceptable and
  clearly labeled; this tier proves transport + geometry + color output + composite, not pixel
  parity. Material may be simplified (single bound albedo OR flat/debug shading) — do NOT block
  Tier 0 on the full texture-array/MaterialGpu path.
- **TIER 1 (follow-up slice, parity):** share the GL scene depth so props depth-test against
  terrain/mechs → true parity → unlocks terrain+mech. BLOCKED on the P5 depth-share problem:
  the engine scene depth is GL_DEPTH24_STENCIL8, proven NOT externally shareable on the 7900 XTX
  (`.claude/VK-ARC-ROADMAP.md` P5/P4 STATUS). Tier 1 must solve shared depth (redirect scene
  depth to D32_SFLOAT_S8_UINT VK-side, or VK depth pre-pass). NOT in this probe.

## Templates (COPY the pattern — both verified this session)
- `GameOS/gameos/vulkan_shadow_island_live.cpp` — geometry-region skeleton: borrow shared
  device (`mc2vk::ensureEngineVulkanContextForInterop` + `getOrCreateEngineVulkanContext`),
  copy shared static-prop VBO/IBO ONCE per map (`upload_geometry_once` + the
  `mc2_shadow_island_get_geometry_feed` / `Mc2ShadowGeomFeed` producer), per-frame instance-SSBO
  memcpy of the `GpuStaticPropInstance` (112B) vector, per-type instanced `vkCmdDrawIndexed`
  with `firstInstance=runStart`, `vkImageResourceBeginFrame` → submit → signal vk2gl →
  `glInteropWaitFromVk`, fence-retire WAW fix, health snapshot + forbidden-readback counters.
- `GameOS/gameos/vulkan_water_island.cpp` — the COLOR half: exportable RGBA16F `VkImageResource`
  + `GlInteropView`, color render pass, and the GL alpha-composite-over-scene shader
  (`composite_water_onto_maincolor`, SRC_ALPHA/ONE_MINUS_SRC_ALPHA). Reuse this composite shape.

## Deltas vs the shadow island (depth-only → color)
1. **Output image:** RGBA16F color (water's format) instead of D32. Usage COLOR_ATTACHMENT|SAMPLED,
   exportable. Plus a VK-OWNED (non-exported) D32 depth attachment for inter-prop depth-test
   (cleared to reverse-Z far each frame — match the GL StaticPropOpaque depth convention; the GL
   prop pass is reverse-Z GEQUAL, NOT the shadow pass's forward-Z LESS — verify in the pipeline).
2. **Render pass:** 1 color attachment (CLEAR to transparent black, STORE, final layout
   SHADER_READ_ONLY for GL sample/composite) + 1 depth attachment (CLEAR, DONT_CARE store).
3. **VS:** port `shaders/static_prop.vert` skinning-free transform — full world→clip MVP
   (`u_worldToClipGL`, GL_FALSE row-major like the GL path) reading `modelMatrix` from the
   instance SSBO; pass through UV (+ normal if lit). NOT the shadow VS's lightSpace matrix.
   Discover + mirror the shared VBO vertex layout (pos/normal/uv offsets) from the GL
   static_prop VAO setup in the batcher — LOCKSTEP the VK vertex-input attributes to it.
4. **FS:** Tier 0 — sample one bound albedo (or flat debug color); output color0. No MRT gbuffer1,
   no objectId, no lighting/IBL/PBR for the probe.
5. **Composite:** at the GL static-prop color draw site (`GpuStaticPropBatcher::flush()` caller),
   when the gate is ON: SKIP the GL `flush()` prop color draws, call the VK island run, then GL
   alpha-composites the VK RGBA16F over the scene color (reuse water's fullscreen-triangle
   composite). Gate-OFF: untouched GL path (byte-identical).

## Feed
Reuse the shadow island's geometry feed (shared VBO/IBO + per-type packets/typeFirstPacket/
typePacketCount) — SAME buffers. For the per-frame color instance set, use the batcher's
visible static-prop instance vector that `flush()` draws (the color pass's instances, NOT the
shadow caster set). If no clean accessor exists, add a minimal read-only feed accessor mirroring
`mc2_shadow_island_get_geometry_feed` (do NOT disturb the GL path).

## Proof (scene-pixel oracle — health counters lie)
- Gate ON, run `view-terrain.bat mc2_01` (or the deployed 0.4c VK exe, `MC2_NO_LAUNCHER=1`),
  FOREGROUND, windowed visible: confirm static props VISIBLY render (color, right silhouettes,
  roughly right positions) — a real scene pixel check, side-by-side vs gate-OFF.
- Forbidden-counter asserts (no color readback on the interop path) == 0, like the shadow island.
- `MC2_VULKAN_VALIDATION=sync` clean.
- Gate-OFF exe byte-identical (`cmp`).
- Tier 0 explicitly does NOT claim pixel parity (occlusion approximate) — do NOT run/￼claim golden
  parity as PASS for Tier 0; that's Tier 1.

## Build / deploy
- `build64_island` (`-DMC2_VULKAN=ON -DMC2_VULKAN_ISLAND=ON`), full relink on layout changes.
- New TU `vulkan_color_prop_island.cpp/.h` — REGISTER in CMake (shadow-island CMake unregistration
  was a known landmine — verify the new TU is actually compiled).
- New VK shaders under `shaders/vulkan/` (`color_prop.vert` / `.frag`) + SPIR-V compile step
  (mirror how shadow_prop.vert.spv is produced).
- Deploy `deploy_payload.py --build-dir build64_island/RelWithDebInfo --build64-root build64_island`
  → `mc2-win64-v0.4c`. NEVER touch 0.5 (concurrent session).
- Register + allowlist + document the gate.

## STOP-gate
Any transport/validation failure or black-scene → gate stays default-OFF, diagnose, do NOT paper
over. Commit early (one committer per worktree; bg VK runs die mid-run → foreground).
