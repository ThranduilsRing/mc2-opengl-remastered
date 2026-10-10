<!-- doc-status: stale | reviewed:2026-10-11 -->
# Pass 3 comment consistency (report-only)

Source: `py -3 tools/comment_tidy/comment_tidy.py --checks CT001,CT010` (JSON: `.claude/cleanup/pass3-comments.json`, 107 findings).
Filtered out: 32 CT001 with `explains_retirement=true` (deliberate retirement notes), and CT010 items that are intentional markers or false positives (listed in the last section).

## CT001 dead-env (remaining, 16 + 1 excluded)

file:line | gate | comment excerpt | proposed edit
--- | --- | --- | ---
GameOS/gameos/debug_state_dump.cpp:372 | MC2_AMBIENT_PROBE | "Runtime ambient cross-check (default-OFF probe MC2_AMBIENT_PROBE)" | Gate not in code. Append " (gate since removed)" or delete the probe sentence.
GameOS/gameos/gos_gpu_sync.h:21 | MC2_GPU_SYNC_ASSERT | "A runtime producer/consumer assert tracker (MC2_GPU_SYNC_ASSERT) is a deliberate..." | Gate absent. Drop the gate name, or append "(gate since removed)".
GameOS/gameos/gos_mech_killswitch.h:123 | MC2_MECH_GPU_PARITY | "MC2_MECH_GPU_PARITY=1 - disable LEAF_SKIP if running parity diagnostic." | Delete the line. Sibling line 133 says the harness was never built.
GameOS/gameos/gos_terrain_water_stream.cpp:113 | MC2_WATER_SHORE_MEANDER_MAXDIST | "(the deferred MC2_WATER_SHORE_MEANDER_MAXDIST)" | Gate absent. Reword to "(deferred; no gate)" or delete the parenthetical.
RenderCore/ambient_contract.h:46 | MC2_AMBIENT_PROBE | "runtime probe (MC2_AMBIENT_PROBE)" | Same as debug_state_dump.cpp:372. Append "(gate since removed)".
code/infowindow.cpp:23 | MC2_HUD_IMGUI_INFOWINDOW | "IN-MISSION-HUD-IMGUI-SCOPE-1 slice D (MC2_HUD_IMGUI_INFOWINDOW)" | Gate absent. Append "(gate since removed)" or delete the parenthetical.
code/infowindowmodel.h:5 | MC2_HUD_IMGUI_INFOWINDOW | "(IN-MISSION-HUD-IMGUI-SCOPE-1 slice D, MC2_HUD_IMGUI_INFOWINDOW)." | Same as infowindow.cpp:23.
code/pausewindow.cpp:25 | MC2_HUD_IMGUI_PAUSE | "nifty slice-B pause panel (MC2_HUD_IMGUI_PAUSE) superseded..." | Describes a superseded gate. Append "(gate since removed)" or delete the gate name.
code/pausewindow.h:43 | MC2_HUD_IMGUI_PAUSE | "NOTE (MERGE-WAVE-1): nifty slice-B pause panel (MC2_HUD_IMGUI_PAUSE, ..." | Same as pausewindow.cpp:25.
mclib/render_contract.cpp:676 | MC2_AMBIENT_PROBE | "Self-gated on MC2_AMBIENT_PROBE." | Gate absent. Delete the "Self-gated" sentence, or append "(gate since removed)".
mclib/render_contract.cpp:1253 | MC2_AMBIENT_PROBE | "self-gated (MC2_AMBIENT_PROBE); runs before order gate" | Same as render_contract.cpp:676.
mclib/terrain_geometry.cpp:213 | MC2_TERRAIN_ADMISSION_MODERN | "Env: MC2_TERRAIN_ADMISSION_MODERN=1 flips to Modern." | Gate absent. Delete the Env line or append "(gate since removed)". Verify the code no longer reads it first.
mclib/terrain_surface_trace.h:15 | MC2_DEBUG_SHADOW_COLLECT | "matches the canonical MC2_DEBUG_SHADOW_COLLECT" | Gate absent. Reword to a non-env idiom reference, or delete.
scripts/mcp/mc2_render_state_server.py:335 | MC2_AMBIENT_PROBE | "Runtime ambient cross-check (MC2_AMBIENT_PROBE). samples>0 proves the probe" | Same as debug_state_dump.cpp:372. Verify whether the script's check still makes sense.
shaders/gpu_driven_terrain_solid.comp:117 | MC2_SOLID_CMD_PATCH_RETIRE | "Bound at slot 8 when MC2_SOLID_CMD_PATCH_RETIRE=1" | Gate absent. Delete the "when" clause, or append "(gate since removed)".
shaders/mech.vert:92 | MC2_GPU_MECH_LIGHTING | "Driven by MC2_GPU_MECH_LIGHTING." | Gate retired to a constant (see gos_mech_batcher.cpp:195). Replace with "(constant; gate retired)".
(excluded) GameOS/gameos/... / mclib/projectz_overlay.cpp:46 | MC2_G / MC2_B / MC2_R | "GL_G=MC2_G, GL_B=MC2_R" | False positive: these are channel names, not gates. No edit.

## CT010 tombstone (proposed deletes: history-only text about code no longer present)

Propose delete the tombstone clause/sentence; keep the current-behavior sentence.

file:line | excerpt | proposed edit
--- | --- | ---
GameOS/gameos/engine_present_vk.cpp:119 | "...used to be its own submit between composite and present" | Delete the "used to be" sentence.
GameOS/gameos/gameos_graphics_bridge.cpp:70 | "...deleted by TERRAIN-BRIDGE-BODY-DELETE-1" | Delete the history clause.
GameOS/gameos/gameos_graphics_capi.cpp:151 | "...was deleted here" (ROAD-PBR-ABANDON-1) | Delete the history clause.
GameOS/gameos/gameosmain.cpp:1268 | "gate deleted" (T3.1) | Delete the history clause.
GameOS/gameos/gos_mech_batcher.cpp:195 | "retired to constant ... guard was removed" | Keep the constant-behavior line, delete the retire history.
GameOS/gameos/gos_postprocess_shadow.cpp:609 | "...was RETIRED" (VK-RUNG8 item-6) | Delete the history clause.
GameOS/gameos/gos_postprocess.h:599 | "The old private-device SSAO/HZB islands were retired..." | Delete the history sentence.
GameOS/gameos/gos_terrain_indirect_atlas.cpp:610 | "The CEMENT-ATLAS-TONE-MATCH-1 and RUNWAY-APRON-TONE-2 passes were deleted 2026-10-05" | Delete the history sentence.
GameOS/gameos/gos_terrain_lod_chunk.cpp:457 | "...removed the edgeStitch[i]==0 capture-predicate exclusion" | Delete the history clause.
GameOS/gameos/vk_image_resource.h:62 | "VK-RUNG8-ITEM2 removed the exportable path" | Delete the history clause.
GameOS/gameos/vulkan_context.cpp:158 | "...interop instance caps were ..." (VK-RUNG8-ITEM3) | Delete the history clause.
GameOS/gameos/vulkan_mech_island.cpp:168 | "MC2_VK_MECH_FEED_NATIVE is RETIRED." | Delete the history sentence.
GameOS/gameos/vulkan_post_hzb_native.cpp:614 | "MC2_VK_POST_HZB_DIRECT_CHAIN is RETIRED. The GL<->VK handoff arm was deleted" | Delete the history sentences.
GameOS/gameos/vulkan_post_screenfx_native.cpp:359 | "direct-chain only ... static-shadow import substrate was retired" | Delete the history clause.
GameOS/gameos/vulkan_post_ssao_native.cpp:687 | "MC2_VK_POST_SSAO_DIRECT_CHAIN is RETIRED. The GL<->VK handoff arm was deleted" | Delete the history sentences.
GameOS/gameos/vulkan_shadow_island_live.cpp:1245 | "(VK-RUNG8 item-4: direct-chain only). The GL<->VK semaphore-handoff arm ... was deleted" | Delete the history clause.
GameOS/gameos/vulkan_shadow_island_live.cpp:1390 | "item-4: ... was deleted" | Delete the history clause.
GameOS/gameos/vulkan_water_island.cpp:669 | "...used to be safe because the previous frame's fence was waited INLINE" | Delete the "used to be" clause.
GameOS/include/gameos.hpp:2366 | "CP-1: ... Body is now just..." (history prefix) | Verify, then delete the CP-1 history prefix.
code/controlgui_render.cpp:435 | "(Removed: blue friendly chevrons...)" | Delete the "(Removed: ...)" note.
code/controlgui_objectives.cpp:883 | "these used to be declared as unsigned" | Delete the "used to be" sentence.
mclib/bdactor.cpp:2050 | "WE have removed the atans here" | Delete the "removed the atans" clause.
mclib/bdactor_render.cpp:635 | "WE have removed the atans here" | Same as bdactor.cpp:2050.
mclib/txmmgr_lights.cpp:733 | "retired: no FNV, no memcmp" | Verify the FNV/memcmp path is gone, then delete.
scripts/provenance_gate.py:783 | "This used to be RECORDED-AT-MINT" | Delete the "used to be" sentence.
scripts/run_smoke.py:1721 | "MC2_CEMENT_EDGE_CURVE was here; the gate was deleted 2026-07-30" | Delete the line.
scripts/run_visual_capture.py:257 | "This used to be os.environ.copy()" | Delete the "used to be" sentence.
shaders/terrain_lod_chunk.frag:1905 | "pow() exponent was deleted here 2026-07-30" | Delete the history sentence.

## CT010 verify before editing (may be current behavior)

- GameOS/gameos/gos_static_prop_batcher_internal.h:229 ("gate plumbing removed"), :9110 ("removed the"), :10086 ("deleted the"): check whether the legacy path still exists.
- GameOS/gameos/gos_terrain_water_stream.cpp:1125 ("dropped - no thin record"): likely a false positive.
- code/missiongui.cpp:4116, :4124 ("Might have been deleted above!!"): stale; delete if the referenced code is absent.
- code/gameobj.cpp:1735 ("put this back in"): stale TODO-style, verify.
- mclib/cliff_decal_tuning.cpp:1 (TERRAIN-DECAL-SLICE-0C): verify the "removed" wording.

## CT010 keep (false positives or intentional markers)

- GameOS/include/memorymanager.hpp:94, :114 ("Used to hold ...") describe struct purpose.
- RenderCore/RendererFeatureRegistry.h:88, :90, :106 ("[Retired]" entries are deliberate deprecation notes).
- GameOS/gameos/gos_terrain_indirect_dispatch.cpp:166 (retirement lifecycle marker by design).
- code/bldng.cpp:1370, code/gvehicl_control.cpp:215 (joke text, no code claim).
- code/brain_special_dispatch.cpp:2544, code/objmgr_update.cpp:1384, code/sensor_trace.h:38 (false positives).
- mclib/stuff/database.cpp:71 (data semantics), mclib/txmmgr_lights.cpp:617 (tracy zone rename note).
- scripts/check-gate-default-agreement.py:1421, scripts/check-sim-identity-propagation.py:293, scripts/smoke_lib/runner.py:125, shaders/gpu_cull.comp:261, tools/class_light_band_harness/class_light_band_harness.cpp:1138 and :2755, tools/repo_intel/dead_gate_scan.py:50, tools/shader_contract_harness/shader_contract_harness.cpp:140 (intentional checks, false positives).
- GameOS/gameos/gos_mech_batcher_api.cpp:199 (current-behavior note).

## Counts

- CT001 total 49: 32 filtered (explains_retirement), 17 remaining, 1 false positive (projectz_overlay.cpp:46), 16 real.
- CT010 total 58: 28 proposed deletes, 8 verify, 22 keep.

Result: NOT EMPTY. 16 CT001 and 28 CT010 deletes (plus 8 to verify) need edits. Report-only, no source files changed.
