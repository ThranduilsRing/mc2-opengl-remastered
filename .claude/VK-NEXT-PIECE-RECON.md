# P2 recon — fog subgraph GL boundaries on the zero-copy seam

**Piece:** put the already-fused VK↔VK postprocess subgraph (edge+oob) onto the interop seam.
**Gate:** `MC2_VULKAN_POSTPROCESS_SUBGRAPH_INTEROP` (default OFF). Reuses piece-#1 + P1.5 primitives.

## Key discovery
The shipped subgraph (`GameOS/gameos/vulkan_postprocess_subgraph.cpp`, gate
`MC2_VULKAN_POSTPROCESS_SUBGRAPH`, ~commit 9f049ce8) ALREADY achieves the VK↔VK internal chain:
**2 Vulkan draws (edge_fog + fog_oob) in ONE render pass, sequential, no internal barrier, ONE
VK-owned shared intermediate color image, ZERO GL round-trip between the two VK passes** (lines
~1014-1023). Equivalence guards replace both GL fog sites (`vkDraws=2, glSkipped=2`).

**BUT its GL boundaries are readback:** INPUT `glGetTexImage(scene color)` → staging → VK upload;
OUTPUT VK result → `vkCmdCopyImageToBuffer` → CPU → `glTexSubImage2D` back to GL.

So P2 = **wrap this proven chain's INPUT+OUTPUT in `VkImageResource`/`GlInteropView`** (zero-copy
scene-color in, result out). The internal chain is already done — this is a boundary-only slice and
the first true rung-2 (VK↔VK chain with zero-copy boundaries) piece.

## Scope
- Files: `vulkan_postprocess_subgraph.cpp` (~293-1078: replace VMA `colorImage` with exportable
  `VkImageResource`, drop the color copy-out, wire GL→VK signal + VK→GL wait using P1.5's back-channel);
  `build_island` (create/destroy shared color resource + GlInteropView at subgraph init/teardown).
- Depth stays readback (blocker #3 / P5). Color-only zero-copy boundaries. Honest latency claim.
- Layout: GL releases shared image in COLOR_ATTACHMENT_OPTIMAL; render-pass expects same; cross-API
  handoff via the P1.5 semaphores, no barrier (mirror the edge-fog layout-stable fix).

## Proof
- Offline transport harness (`tools/vk_interop_transport_test`) FIRST for the RGBA16F format — seconds.
- Then golden-parity: edge+oob color vs the subgraph readback baseline on mc2_01 bookmarks. NOTE: to
  exercise the subgraph readback baseline, use its own gate (`MC2_VULKAN_POSTPROCESS_SUBGRAPH=1`) OFF vs
  interop ON — confirm the exact harness invocation against run_golden_parity like P1 did (feature-gate,
  not necessarily --backend-compare). fog_oob IS in scope here (it's part of the subgraph) — but fog_oob
  is noise-divergent (S5/S6); compare interop-ON vs the subgraph's OWN readback output (not vs GL), so
  fog_oob noise cancels (both sides are the same VK draw, only transport differs). This is the key: the
  oracle is interop-transport-transparency vs the subgraph's own readback, NOT vs GL.
- Forbidden-counters (color): vkCmdCopyImageToBuffer / glGetTexImage(color) / glTexSubImage2D → 0.
- sync-validation clean.

## Conflicts / timing
- Build on POST-P1.5 primitives (VkImageResource now has the VK→GL semaphore back-channel).
- No file overlap with edge-fog island (different TU). build_island init order: exts first, then resource.

## Task breakdown (~9)
T1 gate + exts on the subgraph device (if separate from edge-fog's) → T2 exportable color VkImageResource
+ GlInteropView in build_island → T3 repoint subgraph render-pass color attachment to the shared view →
T4 GL→VK input signal (scene color into shared tex) → T5 drop color copy-out, VK→GL wait → T6
forbidden-counters + health → T7 offline-harness proof → T8 golden-parity vs subgraph readback baseline →
T9 gate register/doc + STOP-gate.
