# Lane 2 s44 cleanup review (248d9a4e6..3ac571c9f)

## Verdict
PASS with should-fix. Net diff is 2 source files (engine_present_vk.cpp, gos_static_prop_batcher_finalize.cpp) plus ledger/state; no TEMP code, no env reads. Both fixes are correct for the default VK path. No must-fix.

## Must-fix
None.

## Should-fix
- Foreign-texture drop mutates `drawData->CmdLists[n]->CmdBuffer` ClipRect in place. If the same draw data is later rendered by the GL3 backend (the forced VK-to-GL fallback in the same frame, or any second consumer), those images are silently gone even though GL names are valid there. Either note the one-shot assumption in the comment or restore/skip-copy. Low risk today (draw data is rebuilt each NewFrame).
- Visible regression is documented only in state (open item 1). Add a one-line note in BUGS row 53 / known_issues that ImGui images (logistics art, HUD icons) are not drawn under Vulkan, so it is not lost.
- The bake duplicates the alpha-test expression already at gos_static_prop_batcher_finalize.cpp ~205-210 and ~808-810 (finalize already computes `packetAlpha` per packet). Reuse one helper so the three cannot drift.
- The GL draw path (gos_static_prop_batcher_flush.cpp:422-441, flush_prep.cpp:205-262) still reads `type.source->listOfTextures` at draw time. Safe only because GL flushes are not run after the type sources are released; the same class of fault is latent if a GL flush ever follows mission teardown. Not a regression; mention in row 53.

## Nits
- `s_packetAlphaBaked` is a file-static in finalize.cpp while the other tables live in gos_static_prop_batcher_state.cpp; clear it in mapload.cpp (where `s_packets.clear()` runs) so it cannot outlive s_packets. It is reassigned at every finalize, so this is tidiness only.
- Comments carry a little journal ("the second campaign mission", fault address). Keep WHAT+WHY: "alpha-test answer is baked because type sources release their texture lists after finalize".
- Two static counters (`s_skipped < 3`, `s_logged < 2`) with different caps; one convention is enough.

## Checked-OK
- Skipping non-font ids: `ImGui_ImplVulkan_AddTexture` is not called anywhere in the repo (only engine_present_vk.cpp touches the backend), so no valid VkDescriptorSet textures exist today besides the font atlas; nothing valid is dropped. Collapsed ClipRect hits `clip_max <= clip_min -> continue` in imgui_impl_vulkan.cpp (1.91.8) before the descriptor bind and draw, so the fault path is avoided. UserCallback and ElemCount==0 commands are correctly left alone. If AddTexture sets are added later, the filter must be widened (the comment states this).
- Liveness guard: ctx, BackendRendererUserData and OwnerViewport->RendererUserData checks are the correct dependencies of RenderDrawData; returns false before any pass is begun.
- Bake: textureAlpha is written only at load/parse time (msl_load.cpp, msl_shape.cpp, tgl_parse.cpp), all before finalize, and the prior code read the same field at the same point in the lifecycle. A mid-mission texture reload does not re-run finalize and the original draw-time read was equally static, so baked == live except in the fault case. Alpha mode changes mid-mission are not a path today; if one is added, rebuild the bake.
- Bounds: `packet >= s_packetAlphaBaked.size()` guard added; table is reassigned each finalize and s_geometryFinalized is reset in mapload.cpp:76 with s_packets.clear(), so no stale table across maps.
- Other VK-island draw-time reads: no `listOfTextures` reference in any vulkan_* or *vk* source; the colour island only calls `mc2_static_prop_packet_alpha_test` (vulkan_color_prop_island.cpp:1671); shadow island uses only geometry_cpu and vertex_stride.

## Proof completeness
- MC2_VK_IMGUI=0 vs default A/B on the old exe not run: acceptable. Diagnosis rests on the guard-passed crash plus foreign_texture_cmds_dropped=41, and the fix run passes (VK campaign leg, tier1 5/5 with validation 0/0). Cheap upgrade if wanted: run the old exe once with the drop disabled and confirm the crash at the logistics screen.
- Late forced VK-to-GL fallback not runnable: not a blocker (VK ImGui is opt-in via MC2_RENDER_BACKEND=vulkan); needs a test gate calling forceGLFallback at a chosen frame before VK becomes default. Combine with the mutation note above.
- First tier1 run had a mc2_03 heartbeat_freeze flake under validation; second run 5/5. Reasonable, but record the flake rate if it recurs.
