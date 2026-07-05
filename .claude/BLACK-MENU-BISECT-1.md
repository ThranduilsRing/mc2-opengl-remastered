# BLACK-MENU-BISECT-1 — root cause: data/defs/ui ↔ exe lockstep failure (NOT a code regression)

Date: 2026-07-05. Isolated worktree `A:/Games/mc2-bisect-tmp`, clean builds (RelWithDebInfo, MC2_IMGUI=ON — required since ui-phase1; configure fails without it). Test install: `A:/Games/mc2-opengl/mc2-win64-v0.4c`. Oracle: launch → restore window → ESC-skip → full-screen grab → mean brightness + frac(px>30); validated VISIBLE on known-good v0.4 exe (menu, mean 63.5 / frac 0.55) before any bisect probe.

## Oracle readings

| Probe | exe (clean build) | data/defs/ui in install | mean | verdict |
|---|---|---|---|---|
| goodv04b | v0.4 known-good | (v0.4's own) | 63.50 | VISIBLE (real menu, screenshot verified) |
| probe63bb | 63bb0832 | stale (0.5-era) | 0.20 | BLACK |
| probe44bb | 44bb10d7 (merge parent) | stale (irrelevant — pre-defs-UI) | 63.51 | VISIBLE |
| probe609c | 609cbd50 (ui-phase1 merge) | stale | 0.19 | BLACK |
| probe609c_anchor0 | 609cbd50 + MC2_UI_ASPECT_ANCHOR=0 | stale | 0.19 | BLACK (killswitch ≠ cure) |
| probe609c_freshdefs | 609cbd50 | **repo-matching defs** | 23.49 | **VISIBLE (full menu, screenshot verified)** |
| probe609c_staledefs_freshui | 609cbd50 | stale defs + **fresh `defs/ui/` only** | 23.42 | VISIBLE → culprit isolated to `data/defs/ui` subtree |
| probe609c_staleui_log | 609cbd50 | fresh defs + stale `ui/` re-swapped | 0.19 | BLACK (control confirms) |
| probe63bb_freshdefs | 63bb0832 | repo-matching defs | 23.42 | **VISIBLE** |

Verdict chain: parent VISIBLE → merge BLACK → merge+matching-data VISIBLE → repo `main` (63bb0832) has **no code regression**. The ui-phase1 merge `609cbd50` introduced a hard data dependency; every "black" sighting was an exe/data version skew.

## Mechanism

1. `609cbd50` (ui-phase1) replaces front-end menu rendering: `LogisticsScreen::render()` black-clears the legacy GameOS HUD layer (`blackClearLegacyLayer()`, gui/logisticsscreen.cpp ~line 545, UI-LAYER-CONTRACT-2) and draws the menu as an ImGui defs page loaded from `data/defs/ui/packages/...`.
2. With a **mismatched** `data/defs/ui` package (the v0.4c dev install carried a NEWER 0.5-era package: extra `ui/fonts/`, `ui/compatibility/`, `mfc_dialogs`, different page .fits), the defs pages report `isLoaded()` (so black-clear fires and legacy widgets are opted out) but render **no visible elements** → screen = opaque black quad. **The UI package loader is completely silent on this** — zero log lines even with MC2_LOG=1.
3. Pre-merge exes (44bb10d7, v0.4) never read defs/ui → immune.
4. Why the external modder saw working menus: clean build + repo data = matching pair. Why user's mc2-setup (5.0 data + 63bb exe) is black: 5.0-era defs/ui ≠ 63bb-era loader — same skew, other direction. Why prior nifty-dirty builds looked black: they ran against installs with skewed defs, not because the objs were hybrid.

## Fix (proposed, NOT committed)

Primary (deployment/discipline): `data/defs/ui` must ship in lockstep with the exe, exactly like shaders. `scripts/deploy_payload.py` should treat `data/defs/` as a versioned payload component; mc2-setup must pair exe+defs from the same commit. Immediate user unblock: copy `data/defs/` from the same source tree the exe was built from into the install.

Secondary hardening (small code diff, high confidence ~90%):
- In `UiDefs`/GuiRuntime page load: log one line per page load failure/empty-page, and add a schema/package version key in `data/defs/ui/packages/default/defines/*.fit`; on mismatch, fall back to `isLegacyPassthrough()` (legacy path renders — degraded but visible) instead of black-clear-with-empty-page. The black-clear should only fire when the page actually produced draw commands this frame (e.g. gate `blackClearLegacyLayer()` on `defsUiPage->renderedElementCount() > 0` from the previous frame).

## Latent bugs found in passing (real, but NOT the black-menu cause)

1. **options.cfg FullScreen parse is case-sensitive** — gameosmain.cpp pre-window resolution bootstrap (ui-phase1): `strstr(eq+1, "true")` misses `b FullScreen = TRUE` (what the game itself writes) → boots `fullscreen=0`. Log evidence: `[RESOLUTION] Pre-window: 3840x2160 fullscreen=0 (from options.cfg)`. Fix: case-insensitive compare / also accept "TRUE".
2. **Intro movie never self-terminates on current builds** — `[INTRO_MOVIE] frame=N` lines run forever (both 44bb10d7 and 609cbd50; v0.4 self-ends). Video master clock (mc2video.cpp `videoMasterClock` → `videoPCMSamplesConsumed()`) appears stuck at 0 → first parked frame is never "due" → movie frozen at (black) frame 0, `video_update` never returns true. Invisible today only because the defs menu draws over it. Also: `gos_GetKeyStatus(KEY_ESCAPE)` stays 0 for synthetic (keybd_event) ESC while game lacks SDL focus — the SW_SHOWMINNOACTIVE boot + `g_focus_lost` filter drop keys until a real focus-gained. Deserves its own slice.
3. **debug_state `ui_pass.draw_count`=0 at menu even when visible** (known) — additionally `frame` advances during the frozen intro, so "frames advance" is NOT evidence the menu was reached.

## Repro/tooling

Oracle script (launch/restore/ESC/grab/kill, tracks its own PID only): scratchpad `oracle.py` of session cc29622a (copy into scripts/ if wanted). Worktree `A:/Games/mc2-bisect-tmp` left at 63bb0832 with built exe; safe to delete via `git worktree remove`.
