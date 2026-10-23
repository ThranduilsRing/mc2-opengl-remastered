# Native Vulkan resource graph (E5)

## RPS comparison (before implementation)

AMD RPS provides dependency compilation, Vulkan barriers, transient-memory
aliasing and scheduling, with an optional RPSL frontend and a visualizer.
Source: [RPS SDK README](https://github.com/GPUOpen-LibrariesAndSDKs/RenderPipelineShaders#introduction),
reviewed 2026-10-23. Its general scheduler and backend would require adapting
our existing command recording and image ownership. That is useful for a broad
renderer migration, but excessive for this packet's single-queue, fixed-order
post chain. Choose a small native graph; no SDK dependency or new feature gate.

Proof chain: scene copy -> SMAA edges -> SMAA weights -> neighborhood blend.
The graph declares image accesses, emits transitions before each pass, and
assigns exact-compatible transient images to nonoverlapping lifetime slots.
SMAA edges and weights overlap in the weights pass, so cannot alias. Correctly
rejecting that reuse matters more than claiming a memory reduction here.

Scope: one graphics queue, whole images, insertion-order execution; no pass
reordering, asynchronous compute, subresource tracking or cross-queue ownership.
Imports retain external ownership. Transient slots reuse an identical image,
not differently formatted images bound to overlapping device memory. Slot
compatibility must describe extent, format, samples, usage and image flags.
Shadow and vegetation remain on their existing path. Future migrations must
declare their full accesses before entering the graph.
