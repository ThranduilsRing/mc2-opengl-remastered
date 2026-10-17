<!-- doc-status: current | written 2026-10-17 (editor lane, session 1) -->
# Editor lane state (branch `claude/editor-vk`, worktree `A:/Games/mc2-lane4`, install `mc2-win64-lane3`)

Goal: GL deletion step 3.6, the mission editor renders its viewport on Vulkan through the SAME path the game uses, so the GL scene code can be deleted once for both hosts.

## Session 1 (Sonnet)

Base: nifty `22a0e8228`. NOTE: lane 2 (`claude/lane2-s3`: 3.1 fatal VK init, 3.2 default flip, 3.4P) is NOT in this base. Merge expectations: `editor_set_default_env_vars` (editor/EditorMFC.cpp) and `initVulkanBackend` (moved here to gos_host_vk.cpp; lane 2 deleted `forceGLFallback` inside it) will conflict textually; resolve by taking lane 2's body of `initVulkanBackend` into `GameOS/gameos/gos_host_vk.cpp` and lane 2's `frameSwapWindow` edits into `presentOrSwap`. After the flip the editor's own `MC2_RENDER_BACKEND` default put can go (backend is vulkan by default) but the `gl` opt-out must keep the umbrella off (see the umbrella note below).

### Harness fixes (tooling commits, proven on the GL editor first)
- `b2875ed69`, `c063feebd`:
  - `run_editor_smoke.py --prepare-stock-scratch` accepts any `A:/Games/mc2-opengl/mc2-win64-lane<N>` install (it already accepted lane3 only); the stock dependency audit then passes against archive-exact loose data. Lane2's refusal was just that flag restriction.
  - ROOT CAUSE of the `-playtest` timeout: the 8 ms render `WM_TIMER` keeps the message queue non-empty, so `EditorMFCApp::OnIdle` (where the driver lived) was starved and never reached `Start()`. The driver is now `EditorPlaytestSmokeDriverTick()`, ticked from `EditorInterface::OnTimer` and `OnIdle`.
  - Second bug behind it: `TacMapTGA::refreshBmp` crashed the playtest's in-place (non-quick) save when the tac map window had never painted (`TGAWnd::m_pBits` uninitialised); initialised and guarded.
  - `run_editor_playtest_smoke.py`: resolves the game from the deploy dir (the editor otherwise probes `..\mc2-win64-v0.4` first), and `--mission stock:mc2_01` names a stock pak materialised loose by `run_editor_smoke.py --prepare-stock-scratch`. Set `MC2_LOG=1` to get the game child's stdout into the archived playtest log.
- Proof (GL editor, lane3): gen_map_basic + stock_roundtrip:mc2_01 PASS (2/2); playtest mc2_01 PASS (editor exit 0, child exit 0, 41 s).

### VK slices
- (a) `45facf7b9`, `1fa791028`, `1410b330f`, `92a76956f`: the editor calls the game's own bring-up and present, now shared in `GameOS/gameos/gos_host_vk.{h,cpp}` (`initVulkanBackend`, `gos::host::presentOrSwap`; moved out of gameosmain_init/gameosmain_frame; the game calls the same functions). `editor_set_default_env_vars` defaults `MC2_RENDER_BACKEND=vulkan` + `MC2_VK_SHARED_GBUFFER=1` (umbrella only when the backend is not `gl`). The editor's VK surface is raw `vkCreateWin32SurfaceKHR` on the viewport child HWND (same as the game's: `SDL_GetWindowWMInfo` works on the `SDL_CreateWindowFrom` window); ImGui renders through the present path's native VK ImGui backend (no editor change). New `-capture=<tga> [-capture-frame=N]` CLI (GL: back-buffer readback; VK: `gos::screenshot::request`) and `scripts/run_editor_capture.py`.
- (b) `8ef026b6b`: RTT viewport (`MC2_EDITOR_RTT`, default off) forced off under VK (it blits the GL back buffer). Panels, outliner selection, inspector, edits all exercised by the smoke list on VK.
- (c) `e5daf9bcd`: playtest passes the editor's backend explicitly to the game child; child verified on VK (archived log with `MC2_LOG=1`: VK present frames, `validation_errors=0`).
- (d) NOT DONE, BLOCKED (see below).

### Proof table (lane3, background, serialized, leased)
| Check | Result |
|---|---|
| EditRel + EditorShell + EditorShellSmoke build (VS tree) | ok at every commit above |
| run_editor_smoke.py, 20 case names incl. 24x stock_roundtrip, on VK | 44/44 PASS; 37/37 kept logs show `[VK_PRESENT] present path UP`, all `validation_errors=0` |
| playtest mc2_01 on VK (editor VK, child VK) | PASS, child exit 0, validation_errors=0 |
| viewport screenshots | `scratch/editor-vk/mc2_01_gl.png` vs `mc2_01_vk.png`; selection `mc2_01_vk_sel.png`; mc2_10 `mc2_10_gl.png` vs `mc2_10_vk.png` (see findings) |
| game after the gos_host_vk move (ninja build at 8ef026b6b, deployed to lane3), run_smoke --lite (mc2_10 + mc2_01), GL and VK | both PASS 2/2, DEPLOY_FINGERPRINT OK |

### Findings
- GL baseline screenshots show scene content the VK ones differ from: VK draws props (trees, buildings) and a lighter terrain; the GL editor path draws terrain + mechs but NO static props on mc2_01/mc2_10. So the VK editor is a visible improvement, not a regression, but props are flat teal on VK (untextured / material fallback). Not investigated (the editor logs `[SPLATTING] FAILED to load all material textures`); first thing to check when texturing is wanted.
- Umbrella note: in this base `MC2_VK_SHARED_GBUFFER=1` with `MC2_RENDER_BACKEND=gl` leaves the scene black (islands own it, nothing presents). The editor default therefore sets the umbrella only when the backend is not `gl`; lane 2's `umbrellaOn()` does that structurally.
- The editor window resize path (swapchain recreate on a live resize) is not exercised by any headless check (the harness window is minimized); the present path itself recreates on extent change (`resize_count` health field).
- MFC child HWND keeps `CS_OWNDC` + WGL pixel format; VK surface on the same HWND coexists exactly like the game's SDL GL window.

### Blocker: slice (d), the editor's private GL context cannot go yet
The game's VK path itself still needs a GL context on the host window: `gos_CreateRenderer`, the GL scene FBOs of `gosPostProcess`, the legacy `gos_*` draw path (HUD, in-scene quads), GL static-prop / terrain-indirect buffers, ImGui texture mirrors keyed by GL names. The editor shares those sources, so removing its WGL context needs the same work as the game's step 3.9 (SDL GL context -> SDL Vulkan window) and 3.5. There is no editor-only part left to delete: `InitEditorOpenGLOnly` does not exist in this tree; the editor's GL bootstrap is `EditorGosRender.cpp` (`create_window` child HWND, `init_render_context` WGL) + `InitGameOS`. Order that works: 3.5 batches shrink the GL scene sources for both hosts (the editor no longer compiles a separate set, it is already on the shared driver `RenderFrameDriver_RenderWorld`), then 3.9 swaps the context.

### What remains for 3.6
1. Texturing / prop parity of the editor viewport on VK vs the game (flat teal props).
2. Live-resize and DPI check by a person (headless harness cannot).
3. (d) rides on 3.5/3.9; at that point delete the WGL context path in `EditorGosRender.cpp`, `RttEnabled` GL blit, `RunGameOSLogic`'s `glReadPixels` object-id pick, and `-capture`'s GL branch.
4. Lane 2 merge conflicts listed above.

### Mergeable tip
Code tip `8ef026b6b` (editor VK slices a-c + tooling); the state-file commit on top of it is the branch head. Branch `claude/editor-vk`.

## Session 2 (Sonnet)

### Item 1: nifty merged (done)
- `a845a78ea` merges `claude/nifty-mendeleev` (31a5ccd48, lane 2 s50-s53). Conflicts: `gameosmain_frame.cpp`, `gameosmain_init.cpp` (took the shared-host side, the moved bodies), `editor/EditorMFC.cpp` (kept the VK editor default; nifty's "editor keeps private GL" default is superseded now that fatal VK init removed the fallback). Lane 2's `forceGLFallback` deletion and fatal-init behavior were applied INSIDE `GameOS/gameos/gos_host_vk.cpp` (`mc2::fatalBackendFailure` at all five former forceGL sites), so game and editor share one path.
- No generated inventory conflicted, so no merge follow-up regeneration was needed.
- Proof: ninja game build at a845a78ea ok (495 steps); deployed to lane3; `run_smoke --lite --deploy lane3` PASS 2/2 (mc2_01 + mc2_10, vk), DEPLOY_FINGERPRINT OK sha a845a78ea50b; VS editor targets EditRel/EditorShell/EditorShellSmoke build ok (build64).
- NOT RUN (blocked): editor smoke and playtest on VK at the merged tree. The editor deploy to lane3 (`deploy_payload.py --target editor`) was denied by the permission classifier in this session, so the merged editor exe is built (`build64/out/editor/RelWithDebInfo/Mission Editor.exe`) but not deployed, and the editor captures were not taken.

### Item 2: viewport parity findings
- ROOT CAUSE of flat teal props AND `[SPLATTING] FAILED`: the lane3 INSTALL, not the editor and not the engine. The GAME on the same install and VK shows the same flat teal buildings (`scratch/editor-vk2/game_bldg/mc2_01_airport_bldgs_mid.png`, viewed) and logs 56 `[TEXMGR_MISSING]` lines (data/tgl/128 A_Hangar, Plates, L_plates, S_wall*, G_stone, ... ) plus `[SPLATTING] NOT FOUND data/textures/mat0_normal.tga`. lane3 `data/tgl/128` holds 660 files against 3570 in v0.4 (tga slimmed, no ktx2 cache), and `data/textures` 312 MB against 1026 MB. Props with no loaded texture land in the `no_primary` teal bucket (`gos_static_prop_batcher_finalize_textures.cpp:417`). The v0.4 golden (`mc2-main/tests/visual/captures/csm_flip1/vk/.../mc2_01_airport_bldgs_mid.png`) shows green textured props and greener terrain from the same code. Fix is data: copy the missing `data/tgl/128/*.tga|ktx2` and `data/textures/mat*_normal.tga` from v0.4 into lane3 (read-only source). The copy was denied by the classifier here; needs the owner's go-ahead. The GL editor "no props" baseline is the same missing-texture condition (GL drops layer=-1 packets, VK draws them teal); not separately proven.
- Terrain "lighter than GL": the game's own VK terrain on lane3 is the same brown tone (`scratch/editor-vk2/game_mc2_01/mc2_01_saved_mechday.png`, `game_mc2_10/mc2_10_saved_mechnight.png`, viewed), so it matches the game; not an editor bug (the missing mat normals plus HDRI EXR failure explain the tone vs v0.4).
- Selection bracket on VK: the code path is already VK-routed (`mclib/appear.cpp` VkHudRouteScope, umbrella). Session 1's `mc2_01_vk_sel.png` selected the first outliner object (Razorback at 2496,-1941), which is not in the camera view, so no bracket would be drawn on either backend. Not proven either way; needs a capture with the camera on the selected unit (`Locate`), blocked on the editor deploy above.

### Item 3: what slice (d) (editor private WGL context) waits on
The editor's GL context is born in `editor/EditorGameOS.cpp:327-336` (`graphics::init_render_context` -> `make_current_context`), `glewInit` at `:345`, `gos_CreateRenderer` at `:368`; the context itself is `SDL_GL_CreateContext` at `GameOS/gameos/gos_render.cpp:637`, shared with the game's `gameosmain.cpp:1178` `gos_CreateRenderer`. It cannot go until the GL scene sources are gone for both hosts (3.5) and the host window becomes a Vulkan window (3.9): `gos_CreateRenderer` (`gameos_graphics.cpp:769`), `gosPostProcess` GL scene FBOs, legacy `gos_*` HUD draw path, GL static-prop/terrain-indirect buffers, ImGui GL texture mirrors. Editor-only GL leftovers to delete in the same step: `-capture` GL branch `editor/EditorGameOS.cpp:507` (`glReadPixels`), `GuiRuntime::RttEnabled()` blit branches `EditorGameOS.cpp:585,795,843`, the object-id pick `EditorGameOS.cpp:881` + `EditorInterface_mouse.cpp:512` (EditorBridge::pickAt glReadPixels), `EditorInterface_internal.h:109-119`.

### Mergeable tip
Branch `claude/editor-vk` at the commit carrying this section; code tip `a845a78ea` (merge). No ratchet rises; no BUGS row added (the defect is an install-data gap, noted above).
