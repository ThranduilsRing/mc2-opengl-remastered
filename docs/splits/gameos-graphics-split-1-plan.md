# GAMEOS-GRAPHICS-SPLIT-1 — extraction plan (recon 2026-07-05)

Source: read-only recon of `GameOS/gameos/gameos_graphics.cpp` (~9k lines). Execute
slices SAFEST-FIRST, one per commit, verify each with dev-shell `framegraph`
(pass list unchanged, GPU ms sane) + single-mission screenshot + bay boot.
Full recon output archived below-in-summary; key facts:

## Ranked slices

| # | Extract | ~Lines | Risk | Coupling notes |
|---|---|---|---|---|
| 1 | gosFont impl → `gameos_graphics_font.cpp` (7053–7789) | 700 | LOWEST | only `getGosRenderer()` in dtor |
| 2 | gosTexture impl + helpers (845–1029, 1043–1162) → `gameos_graphics_texture.cpp` | 185 | LOWEST | local helpers move along (makeKindaSolid/doesLookLikeAlpha/convertIfNecessary) |
| 3 | Overlay batch API (8593–8993) → `gameos_graphics_overlay.cpp` | 400 | LOW | OverlayBatch_ structs → shared `gameos_graphics_internal.h`; NOTE these are gosRenderer members — prefer keeping methods, moving free helpers |
| 4 | Light-matrix SSBO builder (7977–8062) → `gameos_graphics_light_matrix.cpp` | 85 | LOW | pure GL, 2 file-statics move with it |
| 5 | gos_* param accessors (8212–8551) → `gameos_graphics_params.cpp` | 340 | MEDIUM | needs extern s_hud_scale*/g_water* via internal header; verify water path still links |

Total extractable ~1710 L (19%). Core (gosRenderer class, frame pump,
terrain/water/shadow paths) stays for later arcs.

## Landmines
- `g_gos_renderer` singleton read by everything — slices must use `getGosRenderer()`; if core ever splits, singleton gets its own TU.
- `gVAO` MUST stay in the main TU (flushHUDBatch + RebindVAO critical path).
- All classes are TU-internal today (no ODR risk) — moving a class def to a shared internal header must include EVERY field verbatim.
- `#include "gameos_graphics_debug.cpp"` at EOF — keep last in main TU.
- Full-relink rule applies (class-layout unchanged if done right, but delete objs on any doubt).

## Per-slice verification recipe
1. build (documented cmake) 2. deploy 0.4c 3. `mc2_cmd.py framegraph --collect true` in mc2_01 → same pass set, sane GPU ms 4. screenshot scene + bay boot screenshot 5. commit.

## Sequencing note
Do NOT run concurrently with other nifty-worktree agents (shared build64).
TEXTURE-REFRESH-1 agent was in flight 2026-07-05 — check `git log` before starting.
