# Lane 2 s42 cleanup review (92f06782a..ee316b910)

## Verdict
PASS with should-fix. The net diff is clean: no TEMP diagnostic code, env reads or logging remain (14 files, all in stated scope). The row 29 change only runs under `MC2_GPU_PARTICLES` (default OFF) on the VK island, so default-path risk is nil. Neither unproven arm blocks landing; the pull scope below is the one item worth a decision.

## Must-fix
None.

## Should-fix
- The 4-radii pull in `vfx_particle.vert` applies to EVERY particle in every group (alpha smoke/dust included; `max(sX,sY)` is the sprite radius), not just heads or OneOne groups. Large smoke sprites get pulled several metres toward the camera, so nearer geometry no longer occludes them and they can draw in front of buildings or mechs they sit behind. The justification covers only beam heads and impact flares, and the oracle capture shows only beams, so it cannot see this regression. Gate the pull on `G.oneOne` (already in the push block) or on `kind_flags & 1u`, or state in row 29 that smoke occlusion loss is accepted.
- A sprite within 4 radii of the near plane is pulled through it and clips; clamp or note.
- Lazy GL3 init in `NewFrame` is structurally sound (guarded by `!s_openGlRendererInitialized`, flag set once, existing Shutdown tears it down, editor path sets the flag first). It does need a current GL context at the first NewFrame after `forceGLFallback`, and nothing proves the fallback leaves one. Unexercised (see Proof).

## Nits
- `vfx_particle.vert` pull comment is one very long line and carries HOW (cross(right, up) orientation); trim to a two-line WHY.
- `d.additive` now means 0/1/2 with `== 2u` repeated; a named enum would read better. `make_pipeline(additive, oneOne)` allows the invalid pair oneOne && !additive.
- `v_is_head` now carries two bits; rename to `v_flags`.
- The three-line blendMode comment is duplicated in card/cardcloud/pointcloud; acceptable. "the card data ... relies on that" is slightly journal-ish.

## Checked-OK
- GL/MLR for blendMode 2: every GL consumer in the `gos_particle_bridge.cpp` flush (pipeline select, `u_vfxIsAdditive`, blackbody) moved `== 1` to `>= 1`, so GL treats 2 exactly as 1. MLR is untouched; the oracle MLR arm is byte-identical per the state file.
- Pipeline count: one extra graphics pipeline with the same layout, descriptor sets and shared-scene attachments. Push block 20 -> 24 matches the shader and the `static_assert`, vertex stage, far under the 128 B minimum. `oneOnePipe` is in the same `pipeline_create_failed` path and RAII-owned, so no leak. Validation is clean on tier1 (island not exercised there).
- UNORM sampling: the page image is created directly in the UNORM format (no mutable-format view), and sprite pages are private to the VFX island, so no other island user is affected. RGBA8 pages were already UNORM, so only BC7 sRGB changes, aligning it with display-space MLR blending; the decoder returns bytes as stored, consistent with this. It applies to all island groups (alpha too), which matches MLR.
- Frag: OneOne forces a=1 and skips the alpha discard; magenta-key discard and head 1.5x remain. Depth test ON, write OFF, so the pull cannot write depth.
- GuiRuntime: gate cached, backend read live; Render branches on `!s_openGlRendererInitialized` first (fixes the editor case); the three GL3 init sites are mutually exclusive via the flag, so no double init; Shutdown only tears down when initialised. SCRUB keeping `MC2_VK_IMGUI` is correct. The `MC2_ABL_COMPILE_FATAL` verified drop is explained.
- Docs/ledger: tier1 doc journal phrase removed; row 29 and state file consistent. The 14 red contract rows equal the clean 92f06782a baseline and none touch this diff.

## Proof completeness
- GPU arm not run under validation: not a blocker. The gate is default OFF, the tier1 validation covers the default path, and the oracle cannot declare the arm. Do one validation run when that becomes possible, ideally with the pull-scope fix.
- Forced GL fallback not exercised: not a blocker for landing (VK ImGui only on explicit `MC2_RENDER_BACKEND=vulkan`; default backend is GL), but it is the only coverage for the lazy init. Run one late forced fallback before VK becomes the default backend.
