# Lane 2 s41 cleanup review (9c39e5cbf..92f06782a, excluding af42a52e7)

## Verdict
PASS with should-fix. No blocker for the stated scope (default backend is GL, so the flip only changes explicit `MC2_RENDER_BACKEND=vulkan` runs, and those were proven). One real hole: a mid-session fail-soft to GL leaves the run with no ImGui at all, and that is now the default behaviour under VK instead of an opt-in one.

## Must-fix
None.

## Should-fix
- `vkImguiRequested()` is latched once in `GuiRuntime::Init`, but `Init` (gameosmain.cpp, after `initVulkanBackend`) runs before the frame-loop present init, which can still call `mc2::forceGLFallback` (gameosmain_frame.cpp retry-budget path). After that the latch stays 1, the OpenGL3 renderer was never initialised, and `Render()` declines (`renderBackend() != Vulkan` guard) and returns: no ImGui for the rest of the session (pause/info/objective panels, dev shell, inspector). Context-init failure (before `Init`) is fine: the latch resolves 0 and GL ImGui runs. Either initialise the GL3 renderer lazily on fallback, or say in the tier1 doc and gate doc that a late fail-soft drops ImGui. The tier1 doc currently only describes the "not ready" decline. No run exercised this path (`MC2_VK_FORCE_FAIL=1` covers only the early path).
- Editor/`InitEditorOpenGLOnly` initialises the GL3 renderer and sets `s_openGlRendererInitialized`, but `Render()` branches on `vkImguiRequested()` first. With `MC2_RENDER_BACKEND=vulkan` in the environment the editor would now (default ON) take the VK branch with no VK present and draw nothing. Cheap fix that also removes the latch dependence: branch `NewFrame`/`Render` on `s_openGlRendererInitialized` (what was actually initialised) before the request.
- Unrelated gate drift in the same commit: `MC2_ABL_COMPILE_FATAL` lost `verified` in gate_table.cpp, gate_table.json and flags.auto.yaml (`verified_count` 967 -> 966). Nothing in the commit message explains it. Confirm it is an intended generator result or restore it.
- Proof gap that matters: `check-contracts.sh` was not run, and this diff edits the gate table, generated index, registry and flags yaml, which are exactly what the static gate-sync rows check (and the ABL drift above would show there). Run it before landing. GL tier1 does NOT matter here: under GL `vkImguiRequested()` is 0 (backend not Vulkan, or non-`MC2_VULKAN` build), so the GL ImGui path is untouched.

## Nits
- tier1_env_vars.md entry says "default ON since lane 2 session 41": journal phrase; drop it (the default is in the gate row).
- The remaining `mc2::renderBackend() == Vulkan` re-checks in Shutdown/NewFrame/Render are not dead: they are what keeps the post-fallback case from calling VK. If the latch is made dynamic (above), they collapse.
- `scripts/run_vk_field_check.py` SCRUB still lists `MC2_VK_IMGUI`; harmless, but the field env no longer needs the scrub for an ON-by-default gate (it only protects against `=0`).
- debug_renderer.h comment "visible only while the Vulkan ImGui renderer is on (MC2_VK_IMGUI)" is still true; could say "under Vulkan" now that it is the default.

## Checked-OK
- Registry consistency: gate_table.cpp row (Lead0Kill, default true, registry defaultOn true), gate_table.json, gate_index.gen.h (Lead0Kill), RendererFeatureRegistry.h (true), flags.auto.yaml (default true, rule Lead0Kill, default_on true) and the doc string all agree. gate-history.md entry is marked historical.
- Double draw: not possible. GL3 renderer is only initialised in the `else` of `Init`, so VK-requested runs never run `ImGui_ImplOpenGL3_RenderDrawData`; the VK branch always `return`s.
- GL backend (default) and non-`MC2_VULKAN` builds: latch 0, GL ImGui unchanged. Early VK context failure: latch 0, GL ImGui. `=0`: GL ImGui, a real kill-switch.
- Launcher is a separate exe without `GuiRuntime`; headless/offscreen smoke ran VK tier1 under validation (4/5 plus clean rerun, the failure being a documented host-contention fps flake, 0 errors 0 warnings, fingerprint OK x5) and a default-gate ImGui overlay run with a viewed screenshot (`s41-imgui-default.png`, `[VK_IMGUI] requested=1 used_vulkan=1`).
- The s40 review note (transient failure cached) is a WHAT/WHY comment, no `file:NNN`, no journal phrase. New GuiRuntime comment is WHY only.
- VK-PARITY-GAPS row 29 and header are ledger text and consistent with the state file (8,263 / 6,738 / 3,971 / 0.966). Captures viewed per state.
- Minimum viable proof for a VK lane (build, doctest, VK tier1 under validation, capture viewed) is met, apart from check-contracts above.
