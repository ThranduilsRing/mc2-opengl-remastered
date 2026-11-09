# vfx-blend-fold state

Owner symptom: "mc2_10 draws.vfx is about 6.3 per frame against a ceiling of 3.968."
Base: b26fc9bfe; lane: claude/vfx-blend-fold, slot07.

## Decision (orchestrator ruling on alpha)

Audit of scene-colour alpha consumers: none reads it. The only DST_ALPHA factor is the cloud/shoreline multiply
(vulkan_post_screenfx_native.cpp srcAlphaBlendFactor; colour factors DST_COLOR/ZERO, result alpha only). postprocess.frag,
postprocess_visual.frag, aa_fxaa and aa_smaa_blend sample sceneTex as .rgb and write alpha 1.0; HUD/present/capture composite that output.
So RGB equivalence within 1/255 is the bar and modes 0/1/2/5 fold (the alpha channel differs by design).
scripts/tests/test_vfx_scene_alpha_unread.py fails when a dst-alpha consumer appears.

## Implementation

- Indexed path (descriptor indexing; 64-page table per slot, per-record Group SSBO selected through the record's pad word):
  page, sprite rect, atlas columns and blend mode ride per record, so page/rect changes no longer split draws.
- Fold: modes 0/1/2/5 draw through ONE pipeline (ONE, ONE_MINUS_SRC_ALPHA); vfx_particle.frag foldBlend weights rgb and alpha per record.
  Opaque (3) and ONE/SRC_ALPHA (4) keep their pipelines (plan_pipe in vfx_draw_plan.h).
- No sorting: only adjacent equal-key records merge, submission order kept.
- Fallback when the device lacks shaderSampledImageArrayNonUniformIndexing or the page table overflows: the original page-binding shaders/pipelines.
  The capability dumps in A:/Games/mc2-ops/studies/inputs list VK_EXT_descriptor_indexing but not the individual feature bits; the device
  profile requests the bit and the island checks it at init.
- No mclib/gosfx change, no gate, nothing temporal.
