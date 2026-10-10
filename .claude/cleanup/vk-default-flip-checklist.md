<!-- doc-status: current | reviewed:2026-10-11 -->
# VK default-flip checklist (recon, read-only, 2026-10-08)

## 1. Letterbox bars under VK
- Cinematic bars = untextured gos_DrawQuads in code/gamecam.cpp:841-~900 (bars) and :~903 (fadeAlpha quad). Opaque-black via MC2_LETTERBOX_OPAQUE.
- They are NOT drawn after endScene: they are an in-scene immediate draw, and the block never sets gos_State_IsHUD.
  (compare gamecam.cpp:736/740 which wraps the gosFX flush in IsHUD 1/0).
- Under VK the scene color is VK-owned (shared GBuffer). Un-flagged GL quads land in the GL scene FBO that VK discards.
  Only IsHUD draws are buffered (GameOS/gameos/gameos_graphics_draw.cpp:927 drawTris, flushHUDBatch :8234) and queued to the VK present tail
  (queueHudVulkanFrame gameos_graphics.cpp:8644 -> engine_present_vk.cpp:2116, drawn :1640-1700).
  The tail's color-tri pipe handles AlphaInvAlpha untextured quads (vkHudDrawRepresentable :8199 returns true) -> fix is feasible.
- Separate, unrelated: FORCE-43 / OOB-letterbox composite clear+viewport is GL-only (gos_postprocess.cpp:4873-4915,
  gos_Compute43Box GameOS/gameos/gameos_graphics_hud.cpp:955). Grep finds no VK equivalent in engine_present_vk.cpp -> FORCE_43 pillarbox under VK is UNVERIFIED/likely missing.
- Fix sketch: in gamecam.cpp wrap both quad blocks (bars, fade) with gos_SetRenderState(gos_State_IsHUD,1) ... (0), gated
  mc2vkgate::subGate("MC2_VK_VFX") like :736 so GL stays byte-identical. Check z=0.00001/rhw state survives HUD projection remap.
  Verify: ab.py on a mission with a movie-mode intro (mc2_10), frame ~100 with MC2_VISUAL_CAPTURE_CLEAN unset; bars must be opaque black top+bottom.

## 2. Default selection + prior flip
- Selector: GameOS/gameos/render_backend_select.cpp:30 resolveOnce(); unset/empty -> GL. Umbrella MC2_VK_SHARED_GBUFFER opt-in in vk_shared_gbuffer_gate.h umbrellaOn().
- ff459a48a (flip): unset -> vulkan on MC2_VULKAN builds; umbrellaOn default ON when backend Vulkan; RendererFeatureRegistry defaults + docs/flags.auto.yaml flipped. Measured tier1 5/5 PASS, fps VK 129-137 vs GL 79-137.
- abffc9ab7 (revert): VK rendered the wrong viewpoint on mc2_10 frame 900 (low grazing camera, darker; GL = high tactical view).
  Smoke 5/5 only proved VK runs; goldens/visual_verify read the GL FBO so VK was never covered. Quieting+mute changes kept.
  To re-flip: git revert abffc9ab7 (re-applies the 4 files), after gates below.

## 3. Known VK gaps (from VK-PARITY-GAPS.md + HANDOFF-2026-10-08b)
Open: #1 terrain night light pools/grid/cone/realism (L, worst night); #2 mech StandardLit shading (M-L; partly addressed by 4f076ce91 VK-PARITY-MECH-1 - re-verify); #3 terrain ambient floor (S); #4 shadow dist-fade/sun-clamp/terrain guard (M); #5 cement night flat-light + alpha atlas (M); #6 terrain overlay sidecar/decals/runway markings (M-L); #7 water shore edge (M; 47317f8bf ported part of water); #9 terrain relief/cliff/macro/controlmap (M-L, measure defaults); #11 prop fixture billboard (S, verify); #12 SSAO/cloud/HZB opt-in (only if GL enables).
Fixed: #8 fog (96bc2957a), #10 vehicles (F1), trees glass-dither (7b787f937), f6c29dec0 terrain tint defaults off.
Handoff items: shadow-island stride bug (pipeline stride 12 vs 40, vulkan_shadow_island_live.cpp ~:567/:822) fix in flight; mech shadow casters (reverted 7cd819aff/93b36e535/c5e16de6c); dynamic-buildings missing on VK (color-prop island skips dynamic buckets; mc2_10 missing all buildings); letterbox (this doc); golden re-bless under VK; check-test-tier-coverage FAIL rows (confirm pre-existing); FORCE_43 under VK (unverified).
Note: ab.py MAD is not a pass signal; always view montages, A/B one day + one night map.

## 4. Ordered go/no-go checklist (every item must be GO; stop at first NO-GO)
1. VK-aware capture exists. Evidence: capture.py/verify.py read VK present readback (MC2_VK_PRESENT_DUMP_PPM + _DUMP_FRAME), not GL FBO. Cmd: python tools/visual_verify/capture.py --help; confirm output differs when GL FBO blanked.
2. In-flight commits landed + viewed. Evidence: git log shows dyn-buildings feed fix, shadow stride fix; depth PNG coverage ~1% (MC2_VK_SHADOW_MATRIX_DIAG=1).
3. Viewpoint/camera parity (the revert cause). Evidence: ab.py mc2_10 frame 900 montage, same high tactical view. Cmd: python tools/visual_verify/ab.py <mission> --frame 900 (check --help for flags). Also 1 more mission.
4. Buildings/props/trees/mechs/vehicles all present. Evidence: ab.py montage day (e.g. mc2_01) AND night (mc2_10), human-viewed, no missing classes.
5. Shadows: exploded-beam bug gone; mech shadows present or documented accepted gap.
6. Letterbox/fade bars + movie-mode intro render (fix in section 1); FORCE_43=1 pillarbox check under VK.
7. HUD/menus/cursor/briefing/video: ab.py on a menu screen + in-mission HUD; MC2_VK_HUD_DRAW_DUMP shows no [VK_HUD_SKIP_DECLINE] spam.
8. Night gap #1/#5 (+#3 #4): ship-or-accept decision signed by user. NO-GO if night missions visibly differ and not accepted.
9. Water/fog/overlay (#6 #7 #9) accept-or-port decision with ab.py crops.
10. tier1 smoke under VK: MC2_RENDER_BACKEND=vulkan MC2_VK_SHARED_GBUFFER=1 python scripts/smoke_lib/runner.py (tier1, 5/5, 30s) - necessary but NOT sufficient. Record fps vs GL.
11. Fail-soft: VK init failure, minimized-start, and device-lost fall back to GL (VK-FAILSOFT-DISARM-1); run smoke start-minimized.
12. Golden re-bless under VK: tools/visual_verify/bless.py with VK capture, then verify.py passes on both backends; commit goldens with provenance (reset-50331d5 golden is GL-only today).
13. Test tiers: resolve check-test-tier-coverage FAIL rows; gate-default agreement check (flags.auto.yaml vs RendererFeatureRegistry: 0 disagree).
14. Flip: git revert abffc9ab7 (or re-apply ff459a48a 4 files); re-run steps 3, 10, 12 on the flipped default build; keep MC2_RENDER_BACKEND=gl as documented opt-out.
15. Post-flip: ab.py once more on default binary, no env vars, before merging.
