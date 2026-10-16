# Lane 2 s40 cleanup review (9113a4554..07056c242)

## Verdict
PASS. No must-fix.

## Must-fix
None.

## Should-fix
- Transient import failures (staging or device OOM, not a bad texture) are cached as permanent for that texel vector. A later valid state with the same vector is never retried. Acceptable for a default-off island; say so in the row or comment if a retry-on-frame-N policy is wanted.

## Nits
- `failedSource` holds a shared_ptr to each failed texel vector for the island's lifetime, keeping that memory alive. This is also what makes the pointer compare safe against address reuse. Bounded by handle count.
- The cap log is per island instance, so it resets if the island is recreated. Fine ("once per run" in practice).
- Cap-hit handles are not negative-cached; each frame costs a map lookup per such group. Cheap.

## Checked-OK
- Code diff `9113a4554..07056c242` over mclib/ GameOS/ code/ shaders/ contains only fc5ba5e83: vulkan_vfx_island.cpp (cache, cap log), gos_particle_bridge.cpp (comment shortened), plus the `<string>` include in tests/unit/test_screenshot_slot.cpp. No dump, depth-ALWAYS or V-flip code. No `getenv` in vulkan_vfx_island.cpp.
- Negative cache: keyed per handle; the entry compares the texel vector shared_ptr, so a replaced texture (new vector) is retried, which is the "cleared when texels change" behaviour. Failure logs once per vector (log fires only on the attempt). A failed first import frees the descriptor slot.
- 64-page cap: logged once via `pageCapLogged`, not per frame.
- VK-PARITY-GAPS.md: header reads session 40, no dangling parenthesis. Row 29 has the findings, pixel boxes, recipe, `--fixture 1`, and the "whether GL's own depth test also hides the heads" open question. The old session 38 diagnosis block is gone (session 38 remains only as a comparison baseline and in the Total line). Total line says 29 rows, row 29 open, session 40 added no new row.
- Comments state WHAT and WHY; no journal comments in the new code.
