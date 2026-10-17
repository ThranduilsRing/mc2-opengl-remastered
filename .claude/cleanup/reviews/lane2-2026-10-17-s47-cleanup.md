# Lane 2 session 47 cleanup review (9b9270005..8b2eb167b)

## Verdict
Mergeable. No correctness blocker. Two should-fix items (a destroy-before-check ordering, a per-render readback cost to note). Proof gaps do not block this verdict; check-contracts must still run before landing.

## Must-fix
None.

## Should-fix
1. vulkan_mech_island.cpp upload_geometry_once: on a generation change it does waitIdle plus vbo/ibo reset BEFORE checking that feed.vboCpu/iboCpu are non-null. The accessor returns vboCpu==nullptr during a normals rebuild (size mismatch), so the old buffers are destroyed and a device-wide stall is taken, then the function returns false and retries. Move the null/size check above the destroy block. Output is correct today; this only costs an extra stall and a longer no-mech window.
2. gos_EndCameraPreviewRender publishes on EVERY preview render under the umbrella, drawn or not: blit + synchronous glReadPixels (1.9 MB) + fresh 1.9 MB vector, and the new pointer makes the bridge retire and re-upload the VK image every frame it is drawn. Not-shown cost: none (simplecamera.cpp calls End only when it rendered the preview), so cost is bay/mechlopedia/options only. Acceptable; already in Open #1. Pointer identity is the content-change signal, so the buffer cannot be reused; leave unless profiled.

## Nits
- GL state after readback: GL_READ_FRAMEBUFFER stays on the down FBO (End then binds GL_FRAMEBUFFER 0, which resets both: OK), GL_PACK_ALIGNMENT stays 1, GL_PIXEL_PACK_BUFFER forced 0, first call does glBindTexture(GL_TEXTURE_2D,0) on the active unit. invalidateRenderStateCache() runs right after in End; add a one-line comment that this is why no restore is done, or restore PACK_ALIGNMENT.
- Down FBO/texture are never deleted or rebuilt on context loss; fine for process lifetime, say so in a comment.
- s_camPreviewTexelsName is not cleared when the preview colour texture is deleted/recreated on resize; if the old GL name is reused by an ordinary texture, gos_GetCameraPreviewTexels would match it until the next publish. Low probability, one frame. Clear it where the FBO is deleted if trivial.
- The lit-pixel diagnostic (up to 600 full-image scans, s_logged/s_checks) can be dropped once BUGS 56 is closed.
- The "Restore the default framebuffer ..." comment is now stranded above the new VK-PREVIEW-READBACK-1 namespace block instead of above gos_EndCameraPreviewRender. Move it back.
- Committed evidence under .claude/l2s45 and .claude/l2s47 (PNGs and logs, ~2 MB); fine.

## Checked-OK
- Generation bump coverage: bumped with the VBO copy in uploadMechGeometryVbo (used by finalizeGeometry, the late-type rebuild that re-runs finalizeGeometry, and batcher_rebuildMechNormals) and after the IBO upload (geometry.cpp:499). onMapLoad clears staging without bumping but the next successful finalize bumps. The two early-return paths (shader fail, empty staging) do not bump; the island keeps its old buffers and the snapshot is empty or old and consistent with them. Bumps are gated by vkMechGateOn(), so the pure-GL product is unchanged.
- Stale snapshot refusal: get_frame_feed/get_shadow_feed return 0, so the island draws nothing for at most the frames between a rebuild and the next flush() snapshot (snapshotForVkIsland runs every flush, tag copied from the same gen). It never reuses stale ranges. Worst case is a one-frame mech blank at map load, late type, or normals rebuild; invisible at load, a possible single-frame blink on a normals-rebuild UI action.
- Tag is written after runs/instances/bones are filled in the same single-threaded function; no torn tag.
- Re-upload hazard: waitIdle before reset covers all in-flight frames; vbo/ibo are referenced only through vkCmdBind* at the draw site (no cached device address or descriptor); failure path now resets ibo too, geomUploaded stays false so retry works.
- Bridge identity: imguiTexResolve compares the live texel vector pointer and the expired weak_ptr once per frame, so a re-published vector retires the cached set (the s46 check is satisfied). Orientation: readback row 0 = GL bottom row = same as the GL texture the UV crop was authored against; bay2.png shows an upright mech.
- GL backend: publish is behind mc2vkgate::umbrellaOn() (read per call, follows fail-soft); gos_GetTextureTexelsByGlName gains only a cheap name compare; the GL ImGui path never calls it.
- Evidence: tier1 VK 5/5, validation errors 0 warnings 0, campaign 15 missions with 0 robustBufferAccess2, fingerprints match 10897d0fd (.claude/l2s47/t47.log).

## Proof completeness
- check-contracts, contract harnesses and pytest were not run. The diff is GameOS C++ plus docs; rows likely to trip are the cxx17 baseline and inventory/callers ones (new code in gameos_graphics_capi.cpp, vulkan_mech_island.cpp). Does not block this review; DOES block landing on nifty per project rules, so run check-contracts.sh first.
- Mechlopedia and options paint preview not captured: they share the same FBO and the same End function as the bay. Residual risk is a different preview size/scale or End not called on those paths; low. Not blocking; follow-up capture.
