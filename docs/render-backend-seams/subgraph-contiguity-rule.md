# Subgraph Contiguity Rule (SUBGRAPH-CONTIGUITY-GUARD-1)

## The rule (verbatim)

> A Vulkan subgraph may only fuse passes contiguous in canonical GL/frame-graph order.
> Adding a non-contiguous pass is a reorder experiment and must go through
> scheduler/oracle + parity proof as such.

## Why

The shipped Vulkan postprocess subgraph (`VULKAN-POSTPROCESS-SUBGRAPH-1`, `9f049ce8`)
fuses **EdgeFog + OobFog** into one native Vulkan render pass. Those two sub-passes are
**adjacent** in the canonical `endScene()` order, so fusing them changes nothing about
execution order — it is a legal subgraph.

The canonical postprocess sub-pass order in
`GameOS/gameos/gos_postprocess.cpp` `endScene()` is:

```
ScreenShadow -> CloudShadow -> Shoreline -> SSAO -> BoxDecals -> EdgeFog -> OobFog -> Composite
```

The **Cloud-Shadow fork** is the trap. Folding `CloudShadow` into the fog subgraph looks
like a harmless "expansion", but `CloudShadow` is **not** contiguous with `EdgeFog` —
`Shoreline`, `SSAO`, and `BoxDecals` all write scene color *between* them. Pulling
CloudShadow forward into the fog pass (or fog backward to CloudShadow) silently **reorders**
CloudShadow's multiplicative darkening relative to those three passes' writes. That is a
reorder, not a fusion.

The standing precedent for why "no obvious dependency" is **not** a licence to reorder is
the **StaticProp ↔ Mech** experiment: the scheduler oracle judged the swap *legal* (no hard
resource edge), yet the measured parity gate FAILED on `mc2_24` (0.10–0.32% pixel diff,
3–6× the noise floor) because overlapping opaque fragments resolve order-dependently under
a depth-EQUAL tie. The candidate was rejected and its gate left default-OFF permanently.
**Durable lesson: "no hard resource edge" ≠ "visually commutative"; legal verdict ≠
adoption; parity proof beats armchair reasoning.** A non-contiguous subgraph fusion carries
exactly the same risk and must clear the same bar.

## How the constexpr guard enforces it

`RenderCore/postprocess_subpass_order.h` (GL-free / Vulkan-free, proof-only, zero runtime
callers) provides:

- `enum class PostprocessSubPassId { ScreenShadow, CloudShadow, Shoreline, SSAO, BoxDecals,
  EdgeFog, OobFog, Composite }` and `kPostprocessSubPassOrder[]` — the canonical order as the
  single source of truth.
- `constexpr bool vkSubgraphIsContiguous(const PostprocessSubPassId* fusedSet, int n)` —
  returns `true` iff the fused passes' canonical indices form a gap-free run `{min..min+n-1}`
  (`hi - lo == n - 1`, all distinct, all known). A fusion that skips an intervening canonical
  pass returns `false`.
- The shipped fusion is declared as `kShippedPostprocessSubgraph = {EdgeFog, OobFog}` and
  **locked with a `static_assert(vkSubgraphIsContiguous(...))`**. If a future edit adds a
  non-contiguous pass (e.g. CloudShadow) to that set, the build **fails** — forcing the
  change onto the reorder/parity path instead of sneaking in as a subgraph growth.

Coverage (including the negatives) is proven offline by
`tests/unit/test_subgraph_contiguity.cpp`: `{EdgeFog,OobFog}` and `{ScreenShadow,CloudShadow}`
are contiguous; `{CloudShadow,EdgeFog}` and `{ScreenShadow,Shoreline}` are correctly rejected.

**Bottom line:** if `vkSubgraphIsContiguous(candidate)` is false, you are proposing a reorder
— gate it (default-OFF), run it through the scheduler/oracle, and prove pixel parity with
`scripts/run_golden_parity.py` before adopting. Never treat it as a subgraph expansion.
