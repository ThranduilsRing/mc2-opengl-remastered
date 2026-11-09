# vfx-blend-fold state

Owner symptom: "mc2_10 draws.vfx is about 6.3 per frame against a ceiling of 3.968."
Base: b26fc9bfe90a94a60a163d3bc5e2399ab5cbd16d; lane: claude/vfx-blend-fold, slot07.

## Scope and audit

The mixed-blend explanation is the brief's hypothesis until measured on a rendered battle window.
The proposed ONE / ONE_MINUS_SRC_ALPHA fold preserves RGB for modes 0/1/2/5, but fails exact RGBA for 0/1/2.
At fragment alpha 0.5 and destination alpha 0, old/folded alpha is 0.25/0.5 (mode 0),
0.25/0 (mode 1), and 0.5/0 (mode 2). Mode 5 already uses the requested blend equation.
Mode 3 writes depth and disables blending; mode 4 uses ONE/SRC_ALPHA for both channels.
No island mode reads destination alpha as an RGB factor. The downstream screenfx multiply DOES
read destination alpha for its alpha result (vulkan_post_screenfx_native.cpp); it cannot justify
claiming unchanged RGBA. Strict RGBA is the safe default: retain all six blend equations.
The earlier c9b6406c8 spec discusses RGB folding, not preservation of these alpha equations.

All three capability dumps in A:/Games/mc2-ops/studies/inputs advertise VK_EXT_descriptor_indexing,
but omit the Vulkan 1.2 nonuniform sampled-image feature bits. Individual-bit support is therefore
NOT PROVEN by those dumps. The current device profile requires and enables those bits at device birth.

## Implemented, not yet proved

- Descriptor-indexed per-record texture pages and sprite parameters (64-page table per retired call slot).
- Unchanged page-binding shaders/pipelines for feature/limit absence or table overflow.
- No sorting: only adjacent compatible draws merge; geometry and procedural billboards stay separate.
- No mclib/gosfx changes, feature gates or temporal state.
- Existing draw-plan doctests extended for page merging, order, fallback, and blend-equation counterexamples.
- Production indexed shader reflection test pins Group offsets/32-byte stride and the 64-entry sampler array.

## Next steps / receipts

1. Finish indexed shader reflection test (first PC invocation unusually slow; no verdict yet).
2. Commit the change, run box build/unit/shader-layout contracts before runtime proof.
3. Render matching mc2_10 battle windows before/after, report draws.vfx, and capture mc2_10 night plus mc2_01 day.
4. Inspect montages; run final ratchet/lint/precheck and exactly one Windows Ninja compile of mc2 + viewer.
5. Archive any crashes, release slot, report exclusions and NOT RUN rows explicitly.

Mixed additive/alpha -> one draw is deliberately NOT claimed under exact RGBA: the specified
single-source equation cannot represent those stored alpha results. A follow-up needs an explicit
RGB-only contract or a different blend mechanism; do not silently weaken the equivalence test.
