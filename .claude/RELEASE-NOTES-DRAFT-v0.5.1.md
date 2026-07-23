# MC2 OpenGL v0.5.1 — DRAFT release notes (MERGE-WAVE-1, 2026-07-22)

> DRAFT. Do not publish until wave 2 (claude/deferred-sweep-1) lands — it carries the
> shadow-overhaul final state, night lighting pools, runway/transition fix, and water
> edge work listed below; those bullets are staged from the ledger and must be
> re-verified against the actual merged tree before cutting the build.

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

## World rendering (staged for v0.5.1; final state lands with wave 2)

- **Shadow overhaul** — static-map shadow banding cure (macroN receiver-only bandfix) without the blob-shadow regression; short soft caster-hugging shadows, distance-dissolved.
- **Night lighting overhaul** — terrain consumes the light-pool SSBO: warm pools at bases/lamps at night, cement covered, day scenes pixel-identical; bright-concrete-at-night fixed; night ambient floor knob.
- **Missing runways/transition decals restored** — roads/runways authored as cement-alpha transition art with no overlay id are drawn again.
- **Water fixes** — depth-bias linearization (z-fight reduction), water-edge feather default ON, shore sliver fix; water HDRI reflection LOD unified across GL/VK.
- **Camera smoothing** — camera-park fixture + scripted-camera freeze for stable captures; camera smoothing improvements.

## Vulkan backend

- **VK perf at GL parity** — all islands fence-deferred, single vkQueueSubmit present, per-submit fences replace vkQueueWaitIdle, VFX draw coalescing (349 -> 109 peak draws), shared light-feed dedup (LightFeed 0.85 -> 0.43 ms/frame). User-observed 151 FPS; VK ruled closed at GL parity.
- **VKIsl Tracy zones** + tracy_capture_compare harness for future attribution.

## Fixtures / tooling (modder- and dev-facing)

- Quicksave/quickload boot fixtures (`MC2_BOOT_QUICKSAVE_AT` / `MC2_BOOT_QUICKLOAD_AT`), save/load trace log.
- UI command channel (`MC2_UI_COMMAND_FILE`) with acks + screenshot verb; debug view sweep (`MC2_DEBUG_VIEW`).
- Fail-loud asset loading with magenta placeholders (caught 2 real stock-data bugs), mover-registry coherence audit, resolution/aspect contract audit (`MC2_RES_ASSERT`), order-liveness fixtures, path-normalization chokepoint + `fileExists` fix.

## Not in this release

- claude/modding-examples-1 (9 modding tutorials) — deferred (branch sits on the root-docs lineage; merges dirty into nifty).
- claude/deferred-sweep-1 — wave 2; must land before the release build is cut.
