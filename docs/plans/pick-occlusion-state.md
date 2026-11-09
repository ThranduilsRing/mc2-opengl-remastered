# Pick occlusion lane state

Assignment: slot02, claude/pick-occlusion; based on contact-pick-fixes cb52de4e5.
CPU fallback chosen: ID buffer is default-on but only synchronous debug readback
exists; Vulkan mechs and vehicles have no object-ID attachment/writer.
No new GPU synchronization or flags. Existing CURSOR_TARGET diagnostics record
terrain-rejected point picks (object name ID, pixel and the two ray distances).
Shared point picker uses the terrain pick's exact near-plane MC2 ray, intersects
rotated object bounds, rejects hits behind the first visual-heightfield surface,
then ranks terrain-visible movers ahead of overlapping static objects. Depth
resolves candidates within each class. Drag rectangles unchanged.
The ray adapter is a free function in a leaf picking header; Camera's shared
class header is unchanged from the contact-pick base.
Bounds are a CPU approximation, not pixel-exact animated/alpha-tested geometry.
Continuation implemented: restore the stock padded mover rectangle on exact-box
misses using a ray through the type bounds center, terrain-tested on that ray.
Static objects still require exact box hits. Terrain rejection precedes ranking
and records every rejected candidate under CURSOR_TARGET, including tower 30208.
Doctests cover a mover before/partly behind a building, the hill-hidden tower,
and near-miss forgiveness restricted to movers, including terrain rejection.
Next: box build/unit/smoke-lite, targeted contact doctests, boundaries and lint;
one final Windows compile (both deploy targets); box hillside diagnostic/capture
using the existing proof bookmark/input. No sim changes or ms claims.
