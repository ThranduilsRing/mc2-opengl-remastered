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
