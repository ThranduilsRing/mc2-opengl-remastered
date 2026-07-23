# MC2 OpenGL v0.5.1 — release notes (FINALIZED 2026-07-22, MERGE-WAVE-2)

> Canonical copy ships at release_assets/0.5.1/RELEASE_NOTES.md alongside the zips.

## Fixed GitHub issues

- **#42** — downed-mech hover/salvage regression (v0.5.0): salvage cursor + click restored.
- **#50** — hidden link-up mech latch: selection hole + dead gate fixed; `MC2_LINKUP_TRACE` diagnostic added.
- **#43** — After-Action-Report kill-icon strip mis-sampling on column-extended atlases.
- **#34** — uniform near-clip without tessellation (`MC2_TESS` off no longer changes clipping); tess killswitch respected.
- **#49** — windowed boots from options.cfg get a resizable border (residual); already-fixed portion ships with this release.
- **#45 / #8 / #35 / #24** — fixed in prior dev cycles, first shipped in this release (#8 also gains silent purchase-file-open failure tracing).
- **#9** — called-shot effectiveness gate (`MC2_CALLED_SHOT_FIX`, default OFF): head-hit rate 6.6% -> 15.6% when enabled.
- **#17** — fire-refusal instrumentation (`MC2_FIRE_DECISION_TRACE`).
- **#10** — brain-stall classification via extended `MC2_BRAIN_FREEZE_TRACE`.
- **#32** — data-provenance question researched and documented (`docs/data-provenance.md`).

## Display / UI

- **Per-monitor DPI awareness** — menus no longer "zoomed"/cropped at 4K high-DPI (e.g. 225% scaling); DPI awareness asserted as the first statement of `main()`.
- **HiDPI mouse picking** — world/object pick normalizes against SDL window points, fixing the compressed pick zone under display scaling.
- **Pause screen garbage background fixed** — pause frame texture pinned + per-frame GL-name re-resolve (field-caught font-atlas corruption).
- **ImGui HUD panels (opt-in, default legacy)** — in-mission Pause menu (+ abort/exit confirm dialogs), unit Info panel (pixel-matched incl. paperdoll + pilot skills), Objectives overlay, mission Timer, comm Video panel; legacy bitmap fonts rendered pixel-exact in ImGui. Gates: `MC2_UI_IMGUI_PAUSE/INFO/OBJ/TIMER/VIDEO`.
- **Logistics/defs UI fixes** — bay/purchasing/lab list icons, meter gauges, list clipping, briefing text wrap.
- **Launcher encoding** — UTF-8 mod-manifest strings no longer render as mojibake in the launcher (Utf8ToAnsiInPlace + ASCII hard rule for launcher-facing strings).

## World rendering

- **Shadow overhaul** — static-map shadow banding cure (macroN receiver-only bandfix) without the blob-shadow regression; short soft caster-hugging shadows with distance dissolve (`MC2_SHADOW_DIST_FADE_MAX`); donut/continuous shadow series; tree-foliage shadows now alpha-test correctly on the coalesce/BC7-atlas path too (`MC2_SHADOW_PROP_ALPHA_ATLAS`, default ON — atlas trees no longer cast solid square shadows).
- **Night lighting overhaul** — terrain + road/runway/cement decals consume the light-pool SSBO (`MC2_TERRAIN_LIGHT_POOLS`): warm pools at bases/lamps at night, mover-attached light handling (dim/scale/max-blend), tone-compression; day scenes pixel-identical.
- **Cement night light** — runway/road decals and cement transition rings no longer stay full-bright on night missions (`MC2_CEMENT_MISSION_LIGHT`, default ON; `=0` restores legacy).
- **Pad-area glow** — landing pads get a feathered night lift shared between chunk terrain and decals (`MC2_PAD_AREA_GLOW`, spill via `MC2_PAD_AREA_GLOW_SPILL`).
- **Light cones** — authored cone aim/spill + additive cone glow on light-carrying models (LIGHT-CONE-MODELS-1).
- **Missing runways/transition decals restored** — roads/runways authored as cement-alpha transition art with no overlay id are drawn again.
- **Water fixes** — depth-bias linearization (z-fight reduction), water-edge feather default ON, shore sliver fix + shore mask; projection-aspect-correct reflection gradients (fixes wave-reflection distortion at non-4:3 window aspects); water HDRI reflection LOD unified across GL/VK.
- **Camera smoothing** — frame-rate-independent exponential smoothing for keyboard/edge scroll, rotation snaps, and wheel zoom (`MC2_CAMERA_SMOOTH=<ms>`, default 100; `=0` restores legacy hard steps). Camera-park fixture for stable captures.

## Vulkan backend

- **VK perf at GL parity** — all islands fence-deferred, single vkQueueSubmit present, per-submit fences replace vkQueueWaitIdle, VFX draw coalescing (349 -> 109 peak draws), shared light-feed dedup (LightFeed 0.85 -> 0.43 ms/frame). User-observed 151 FPS; VK ruled closed at GL parity.
- **VKIsl Tracy zones** + tracy_capture_compare harness for future attribution.

## Modding

- **9 worked modding tutorials** (`docs/modding/examples/`): getting-started, mechanism map, texture-override, weapon-edit, weapon-add, variant-add, mech-import, building-import, tree-import, imgui-editing, mission-edit — each with a real, installable payload.
- **Reproducible mod packs** — `tools/mc2mod` pack now pins zip timestamps: identical payload => identical sha256.
- **Example mods ship in the engine zip** (`mods/`): magic-ballistic-weapons, magic-gamesys-tweaks, modern-tree-pack-v1, my-explosions.
- **mc2-launcher.exe ships in the engine zip** — the documented front door (campaign/mod picker).

## Fixtures / tooling (modder- and dev-facing)

- Quicksave/quickload boot fixtures (`MC2_BOOT_QUICKSAVE_AT` / `MC2_BOOT_QUICKLOAD_AT`), save/load trace log.
- UI command channel (`MC2_UI_COMMAND_FILE`) with acks + screenshot verb; debug view sweep (`MC2_DEBUG_VIEW`).
- Fail-loud asset loading with magenta placeholders (caught 2 real stock-data bugs), mover-registry coherence audit, resolution/aspect contract audit (`MC2_RES_ASSERT`), order-liveness fixtures, path-normalization chokepoint + `fileExists` fix.

## Not in this release

- Nothing deferred from the v0.5.1 merge waves — all staged branches (issue-fixes, ui-phase1, vk-interop, road-pbr-abandon, unit-profile-seam, deferred-sweep-1) and the modding-tutorial cherry-picks landed.
