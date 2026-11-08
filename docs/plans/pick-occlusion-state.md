# Pick occlusion lane state

Assignment: slot03, claude/pick-occlusion; based on contact-pick-fixes cb52de4e5.
CPU fallback chosen: ID buffer is default-on but only synchronous debug readback
exists; Vulkan mechs and vehicles have no object-ID attachment/writer.
No new GPU synchronization or flags. Existing CURSOR_TARGET diagnostics record
terrain-rejected point picks (object name ID, pixel and the two ray distances).
Shared point picker uses the terrain pick's exact near-plane MC2 ray, intersects
rotated object bounds, rejects hits behind the first visual-heightfield surface,
and resolves foreground depth before selection priority. Drag rectangles unchanged.
Bounds are a CPU approximation, not pixel-exact animated/alpha-tested geometry.
Added ridge/front/rear and ray-box miss doctests in test_contact_pick.cpp.
Next: review diff; targeted contracts/doctests; box build/unit/smoke-lite; final
Windows compile/lint; mc2_03 hillside bookmark/check. No ms claims.
