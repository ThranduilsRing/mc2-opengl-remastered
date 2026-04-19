# HUD / Scene Split Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move all HUD, text, cursor, mission markers, and nav beacons to render after the post-process composite so bloom/FXAA/tonemapping no longer apply to UI.

**Architecture:** Add `gos_State_IsHUD` to the render-state enum. When set, `drawQuads()`/`drawLines()`/`drawText()` buffer their draw calls (vertex payload + full render-state snapshot + projection) instead of submitting to GL. After `pp->endScene()` composites the scene to framebuffer 0, a new `gos_RendererFlushHUDBatch()` replays all buffered calls in order to FB 0 using the same material-selection/state-application logic as immediate draws. Game code wraps HUD draw blocks with explicit `gos_SetRenderState(gos_State_IsHUD, 1/0)`.

**Tech Stack:** OpenGL 4.2, C++11, SDL2, Tracy profiler. Build: CMake RelWithDebInfo. Deploy via `/mc2-deploy` skill.

**Design doc:** `docs/superpowers/specs/2026-04-18-hud-scene-split-design.md`

---

## File Map

| File | What changes |
|---|---|
| `GameOS/include/gameos.hpp` | Add `gos_State_IsHUD` to `gos_RenderState` enum |
| `GameOS/gameos/gameos_graphics.cpp` | `HudDrawCall` struct; `hudBatch_`/`hudFlushed_` members; `beginFrame()` clear + hygiene assertion; `drawQuads()`/`drawLines()` buffering; `drawText()` T2 buffering; `flushHUDBatch()`; `replayTextQuads()`; `gos_RendererFlushHUDBatch()` extern |
| `GameOS/gameos/gameosmain.cpp` | `extern gos_RendererFlushHUDBatch()`; call after `pp->endScene()` |
| `code/mechcmd2.cpp` | `IsHUD` wrapping on `DEBUGWINS_render()`, `userInput->render()`, status-bar text draws |
| `code/missiongui.cpp` | `IsHUD` wrapping on mission marker/nav-beacon draw blocks (Task 10) |
| `code/objective.cpp` | `IsHUD` wrapping on mission marker/nav-beacon text draws (Task 10) |
| `code/mechicon.cpp` | `IsHUD` wrapping on force-group / unit icons (Task 10) |
| `code/controlgui.cpp` | `IsHUD` audit (Task 10) |
| `code/gametacmap.cpp` | `IsHUD` audit (Task 10) |

---

## Task 1: Add `gos_State_IsHUD` to the Render State Enum

**Files:**
- Modify: `GameOS/include/gameos.hpp` (around line 2113)

- [ ] **Step 1: Open gameos.hpp and find the enum insertion point**

  Confirm the existing tail of `gos_RenderState`:
  ```
  grep -n "gos_State_Overlay\|gos_MaxState" GameOS/include/gameos.hpp
  ```
  Expected output shows `gos_State_Overlay` then `gos_MaxState`.

- [ ] **Step 2: Insert `gos_State_IsHUD` before `gos_MaxState`**

  In `GameOS/include/gameos.hpp`, change the tail of the `gos_RenderState` enum from:
  ```cpp
  	gos_State_Overlay,			// Default: 0						true/false - overlay with GPU projection + shadow sampling

  	gos_MaxState				// Marker for last render state
  ```
  to:
  ```cpp
  	gos_State_Overlay,			// Default: 0						true/false - overlay with GPU projection + shadow sampling

  	gos_State_IsHUD,			// Default: 0						true/false - buffer draw for post-postprocess HUD replay

  	gos_MaxState				// Marker for last render state
  ```

- [ ] **Step 3: Build to verify the enum change compiles cleanly**

  ```bash
  cd A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev
  "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" \
    --build build64 --config RelWithDebInfo 2>&1 | tail -20
  ```
  Expected: build succeeds. `gos_MaxState` now covers one more slot — all existing code using `gos_MaxState` arrays picks up the new size automatically.

- [ ] **Step 4: Commit**

  ```bash
  git add GameOS/include/gameos.hpp
  git commit -m "feat: add gos_State_IsHUD to gos_RenderState enum"
  ```

---

## Task 2: Add HUD Batch Data Structures to `gosRenderer`

**Files:**
- Modify: `GameOS/gameos/gameos_graphics.cpp`

- [ ] **Step 1: Add `HudDrawCall` struct before the `gosRenderer` class**

  Find the comment `////////////////////////////////////////////////////////////////////////////////` before `class gosRenderer {` (around line 1040). Insert above it:

  ```cpp
  enum HudDrawKind { kHudQuadBatch, kHudLineBatch, kHudTextQuadBatch };

  struct HudDrawCall {
      HudDrawKind              kind;
      std::vector<gos_VERTEX>  vertices;
      uint32_t                 stateSnapshot[gos_MaxState];
      mat4                     projection;
      DWORD                    fontTexId;       // kHudTextQuadBatch only
      DWORD                    foregroundColor; // kHudTextQuadBatch only
  };
  ```

- [ ] **Step 2: Add member variables to `gosRenderer`'s private section**

  In `gameos_graphics.cpp`, find the private member block near line 1453 (after `break_draw_call_num_`). Add:

  ```cpp
          // HUD command buffer
          std::vector<HudDrawCall> hudBatch_;
          bool                     hudFlushed_;
  ```

- [ ] **Step 3: Add method declarations to `gosRenderer`'s private section**

  In the same private section (near the existing `void drawQuads(...)` declarations around line 1237), add:

  ```cpp
          void flushHUDBatch();
          void replayTextQuads(const HudDrawCall& call);
  ```

- [ ] **Step 4: Initialize `hudFlushed_` in the constructor**

  In the `gosRenderer` constructor (around line 1049), add to the initializer body:
  ```cpp
          hudFlushed_ = false;
  ```

- [ ] **Step 5: Build to verify the struct and member additions compile**

  ```bash
  "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" \
    --build build64 --config RelWithDebInfo 2>&1 | tail -20
  ```
  Expected: clean build.

- [ ] **Step 6: Commit**

  ```bash
  git add GameOS/gameos/gameos_graphics.cpp
  git commit -m "feat: add HUD batch data structures to gosRenderer"
  ```

---

## Task 3: Clear Batch in `beginFrame()` with Hygiene Assertion

**Files:**
- Modify: `GameOS/gameos/gameos_graphics.cpp`

- [ ] **Step 1: Replace `gosRenderer::beginFrame()` body**

  Find the current `beginFrame()` (around line 2058):
  ```cpp
  void gosRenderer::beginFrame()
  {
      glBindVertexArray(gVAO);
      num_draw_calls_ = 0;
  }
  ```

  Replace with:
  ```cpp
  void gosRenderer::beginFrame()
  {
      // Frame-boundary hygiene: IsHUD must be cleared by every callsite before the frame ends.
      // If it is still set here, a callsite leaked the bit across the frame boundary.
      if (renderStates_[gos_State_IsHUD] != 0) {
          SPEW(("GRAPHICS", "[HUD] gos_State_IsHUD still set at frame start -- callsite leak\n"));
          renderStates_[gos_State_IsHUD] = 0;
      }
      hudBatch_.clear();
      hudFlushed_ = false;
      glBindVertexArray(gVAO);
      num_draw_calls_ = 0;
  }
  ```

- [ ] **Step 2: Build and verify**

  ```bash
  "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" \
    --build build64 --config RelWithDebInfo 2>&1 | tail -20
  ```
  Expected: clean build.

- [ ] **Step 3: Commit**

  ```bash
  git add GameOS/gameos/gameos_graphics.cpp
  git commit -m "feat: clear HUD batch in beginFrame() with hygiene assertion"
  ```

---

## Task 4: Buffer `drawQuads()` and `drawLines()` When `IsHUD` Is Set

**Files:**
- Modify: `GameOS/gameos/gameos_graphics.cpp`

- [ ] **Step 1: Add HUD buffering path to `drawQuads()`**

  Find `void gosRenderer::drawQuads(gos_VERTEX* vertices, int count)` (around line 2159). Insert immediately after `gosASSERT(vertices);` and before `if(beforeDrawCall()) return;`:

  ```cpp
      if (renderStates_[gos_State_IsHUD]) {
          if (hudFlushed_) {
              SPEW(("GRAPHICS", "[HUD] Late drawQuads discarded (after flushHUDBatch)\n"));
              return;
          }
          HudDrawCall call;
          call.kind = kHudQuadBatch;
          call.vertices.assign(vertices, vertices + count);
          memcpy(call.stateSnapshot, renderStates_, sizeof(call.stateSnapshot));
          call.projection = projection_;
          call.fontTexId = 0;
          call.foregroundColor = 0;
          hudBatch_.push_back(std::move(call));
          return;
      }
  ```

- [ ] **Step 2: Add HUD buffering path to `drawLines()`**

  Find `void gosRenderer::drawLines(gos_VERTEX* vertices, int count)` (around line 2205). Insert immediately after `gosASSERT(vertices);` and before `if(beforeDrawCall()) return;`:

  ```cpp
      if (renderStates_[gos_State_IsHUD]) {
          if (hudFlushed_) {
              SPEW(("GRAPHICS", "[HUD] Late drawLines discarded (after flushHUDBatch)\n"));
              return;
          }
          HudDrawCall call;
          call.kind = kHudLineBatch;
          call.vertices.assign(vertices, vertices + count);
          memcpy(call.stateSnapshot, renderStates_, sizeof(call.stateSnapshot));
          call.projection = projection_;
          call.fontTexId = 0;
          call.foregroundColor = 0;
          hudBatch_.push_back(std::move(call));
          return;
      }
  ```

- [ ] **Step 3: Build and verify**

  ```bash
  "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" \
    --build build64 --config RelWithDebInfo 2>&1 | tail -20
  ```
  Expected: clean build.

- [ ] **Step 4: Commit**

  ```bash
  git add GameOS/gameos/gameos_graphics.cpp
  git commit -m "feat: buffer drawQuads/drawLines when gos_State_IsHUD is set"
  ```

---

## Task 5: Buffer `drawText()` with T2 Pre-Expansion

**Files:**
- Modify: `GameOS/gameos/gameos_graphics.cpp`

`gos_TextDraw()` → `gosRenderer::drawText()`. When `IsHUD` is set: run glyph layout as normal (writes to `text_->`), capture the expanded vertex payload, push a `kHudTextQuadBatch` entry, rewind `text_->` without drawing. Layout runs at buffer time; replay submits the already-expanded vertices.

- [ ] **Step 1: Insert T2 buffering branch in `drawText()`**

  In `gosRenderer::drawText()` (around line 3081), find the point after the glyph layout loop ends and BEFORE the GL state override block. The layout loop ends where `pos += num_chars;` and the closing `}` of the `while(pos < count)` loop is. The override block starts with `int prev_texture = getRenderState(gos_State_Texture);`.

  Insert between the loop's closing `}` and `int prev_texture`:

  ```cpp
      // T2: if HUD buffering is active, capture pre-expanded glyph geometry and defer
      if (renderStates_[gos_State_IsHUD]) {
          const int n = text_->getNumVertices();
          if (n > 0 && !hudFlushed_) {
              HudDrawCall call;
              call.kind = kHudTextQuadBatch;
              call.vertices.assign(text_->getVertices(), text_->getVertices() + n);
              memcpy(call.stateSnapshot, renderStates_, sizeof(call.stateSnapshot));
              call.projection = projection_;
              call.fontTexId = tex_id;
              call.foregroundColor = ta.Foreground;
              hudBatch_.push_back(std::move(call));
          } else if (hudFlushed_) {
              SPEW(("GRAPHICS", "[HUD] Late drawText discarded (after flushHUDBatch)\n"));
          }
          text_->rewind();
          return;
      }
  ```

- [ ] **Step 2: Build and verify**

  ```bash
  "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" \
    --build build64 --config RelWithDebInfo 2>&1 | tail -20
  ```
  Expected: clean build.

- [ ] **Step 3: Commit**

  ```bash
  git add GameOS/gameos/gameos_graphics.cpp
  git commit -m "feat: buffer drawText() with T2 pre-expansion when gos_State_IsHUD is set"
  ```

---

## Task 6: Implement `flushHUDBatch()` and `replayTextQuads()`

**Files:**
- Modify: `GameOS/gameos/gameos_graphics.cpp`

- [ ] **Step 1: Add `replayTextQuads()` implementation**

  Add after `gosRenderer::drawText()` (after its closing `}`, around line 3210):

  ```cpp
  void gosRenderer::replayTextQuads(const HudDrawCall& call)
  {
      if (call.vertices.empty()) return;

      text_->addVertices(const_cast<gos_VERTEX*>(call.vertices.data()),
                         (int)call.vertices.size());

      int prev_texture = getRenderState(gos_State_Texture);
      setRenderState(gos_State_Texture, call.fontTexId);
      setRenderState(gos_State_Filter, gos_FilterNone);

      applyRenderStates();
      gosRenderMaterial* mat = text_material_;

      vec4 fg;
      fg.x = (float)((call.foregroundColor & 0xFF0000) >> 16) / 255.0f;
      fg.y = (float)((call.foregroundColor & 0xFF00)   >>  8) / 255.0f;
      fg.z = (float)( call.foregroundColor & 0xFF)            / 255.0f;
      fg.w = 1.0f;
      mat->getShader()->setFloat4(s_Foreground, fg);

      mat->setTransform(projection_);   // projection_ is the captured one, set by flushHUDBatch
      mat->setFogColor(fog_color_);
      text_->draw(mat);
      text_->rewind();

      setRenderState(gos_State_Texture, prev_texture);
  }
  ```

- [ ] **Step 2: Add `flushHUDBatch()` implementation**

  Add immediately after `replayTextQuads()`:

  ```cpp
  void gosRenderer::flushHUDBatch()
  {
      if (hudBatch_.empty()) {
          hudFlushed_ = true;
          return;
      }

      // Ensure we draw to the default framebuffer at full screen resolution.
      // pp->endScene() should leave us here, but post-process viewport changes
      // (half-res bloom, etc.) are not tracked in renderStates_.
      glBindFramebuffer(GL_FRAMEBUFFER, 0);
      glViewport(0, 0, (GLsizei)width_, (GLsizei)height_);

      // Save pre-flush render state and projection
      uint32_t priorState[gos_MaxState];
      memcpy(priorState, renderStates_, sizeof(priorState));
      mat4 priorProjection = projection_;

      for (const HudDrawCall& call : hudBatch_) {
          memcpy(renderStates_, call.stateSnapshot, sizeof(renderStates_));
          renderStates_[gos_State_IsHUD] = 0;   // clear to prevent re-buffering on replay
          projection_ = call.projection;

          switch (call.kind) {
              case kHudQuadBatch:
                  drawQuads(const_cast<gos_VERTEX*>(call.vertices.data()),
                            (int)call.vertices.size());
                  break;
              case kHudLineBatch:
                  drawLines(const_cast<gos_VERTEX*>(call.vertices.data()),
                            (int)call.vertices.size());
                  break;
              case kHudTextQuadBatch:
                  replayTextQuads(call);
                  break;
          }
      }

      // Restore pre-flush render state and projection
      memcpy(renderStates_, priorState, sizeof(priorState));
      projection_ = priorProjection;
      hudFlushed_ = true;
  }
  ```

- [ ] **Step 3: Add `gos_RendererFlushHUDBatch()` extern near `gos_RendererBeginFrame()`**

  Find `gos_RendererEndFrame()` (around line 3239). Add immediately after it:

  ```cpp
  void gos_RendererFlushHUDBatch() {
      gosASSERT(g_gos_renderer);
      g_gos_renderer->flushHUDBatch();
  }
  ```

- [ ] **Step 4: Build and verify**

  ```bash
  "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" \
    --build build64 --config RelWithDebInfo 2>&1 | tail -20
  ```
  Expected: clean build.

- [ ] **Step 5: Commit**

  ```bash
  git add GameOS/gameos/gameos_graphics.cpp
  git commit -m "feat: implement flushHUDBatch(), replayTextQuads(), gos_RendererFlushHUDBatch()"
  ```

---

## Task 7: Wire `gos_RendererFlushHUDBatch()` in `draw_screen()`

**Files:**
- Modify: `GameOS/gameos/gameosmain.cpp`

- [ ] **Step 1: Add extern declaration near the existing renderer externs**

  Find (around line 23):
  ```cpp
  extern void gos_RendererBeginFrame();
  extern void gos_RendererEndFrame();
  ```
  Add below:
  ```cpp
  extern void gos_RendererFlushHUDBatch();
  ```

- [ ] **Step 2: Call flush after `pp->endScene()` in `draw_screen()`**

  Find `draw_screen()` (around line 220). The current sequence ending:
  ```cpp
      // Composite post-processed scene to default framebuffer
      if (pp) {
          pp->endScene();
      }
  ```
  Change to:
  ```cpp
      // Composite post-processed scene to default framebuffer
      if (pp) {
          pp->endScene();
      }

      // Replay buffered HUD draws to FB 0 (after post-process)
      gos_RendererFlushHUDBatch();
  ```

- [ ] **Step 3: Build**

  ```bash
  "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" \
    --build build64 --config RelWithDebInfo 2>&1 | tail -20
  ```
  Expected: clean build.

- [ ] **Step 4: Deploy and smoke-test**

  Run `/mc2-build-deploy` skill or manually:
  ```bash
  cp -f build64/RelWithDebInfo/mc2.exe "A:/Games/mc2-opengl/mc2-win64-v0.1.1/mc2.exe"
  ```
  Launch the game. **No IsHUD callsites exist yet, so visual output must be identical to before.** Verify no crash, no visual change. If the hygiene SPEW fires in the console, something unexpected sets IsHUD — investigate before proceeding.

- [ ] **Step 5: Commit**

  ```bash
  git add GameOS/gameos/gameosmain.cpp
  git commit -m "feat: wire gos_RendererFlushHUDBatch() in draw_screen() after endScene()"
  ```

---

## Task 8: Wrap Screen-Space HUD Callsites in `mechcmd2.cpp`

**Files:**
- Modify: `code/mechcmd2.cpp`

These callsites are verified HUD-only (no scene content). Function-level wrapping is correct for each.

- [ ] **Step 1: Wrap `DEBUGWINS_render()` and `userInput->render()` in `UpdateRenderers()`**

  In `code/mechcmd2.cpp`, find `UpdateRenderers()` (line 686). Locate this block (around lines 750-760):
  ```cpp
  		{
  			ZoneScopedN("UpdateRenderers uiRender");
  			gos_SetRenderState( gos_State_Filter, gos_FilterNone );
  			userInput->render();
  		}

  		{
  			ZoneScopedN("UpdateRenderers debugWindows");
  			DEBUGWINS_render();
  		}
  ```

  Change to:
  ```cpp
  		{
  			ZoneScopedN("UpdateRenderers uiRender");
  			gos_SetRenderState( gos_State_Filter, gos_FilterNone );
  			gos_SetRenderState( gos_State_IsHUD, 1 );
  			userInput->render();
  			gos_SetRenderState( gos_State_IsHUD, 0 );
  		}

  		{
  			ZoneScopedN("UpdateRenderers debugWindows");
  			gos_SetRenderState( gos_State_IsHUD, 1 );
  			DEBUGWINS_render();
  			gos_SetRenderState( gos_State_IsHUD, 0 );
  		}
  ```

- [ ] **Step 2: Wrap the status-bar and debug-text draws in `UpdateRenderers()`**

  Find the `gos_TextDraw` block in `UpdateRenderers()` (around lines 525-562 inside the debug window rendering). These all go through `gos_TextDraw()` and `gos_TextSetAttributes()` — pure HUD text. Locate the opening `if (DebugStatusBarOpen && DebugStatusBarString[0])` block and any adjacent `gos_TextDraw` calls. Wrap the entire text block:

  ```cpp
  		gos_SetRenderState( gos_State_IsHUD, 1 );
  		gos_TextSetAttributes(DebugWindow[0]->font, 0xffffffff, 1.0, true, true, false, false);
  		gos_TextSetRegion(0, 0, Environment.screenWidth, Environment.screenHeight );
  		gos_TextSetPosition(15, 10);
  		if (DebugStatusBarOpen && DebugStatusBarString[0])
  			gos_TextDraw(DebugStatusBarString);
  		// ... rest of debug text draws ...
  		gos_SetRenderState( gos_State_IsHUD, 0 );
  ```

  Read the exact block at lines 523-562 in `mechcmd2.cpp` before editing to confirm its boundaries. Wrap the entire contiguous `gos_TextDraw` group, not individual calls.

- [ ] **Step 3: Build and deploy**

  ```bash
  "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" \
    --build build64 --config RelWithDebInfo 2>&1 | tail -20
  cp -f build64/RelWithDebInfo/mc2.exe "A:/Games/mc2-opengl/mc2-win64-v0.1.1/mc2.exe"
  ```

- [ ] **Step 4: Visual check — HUD text should be unchanged, no bloom on debug text**

  Enable bloom (RAlt+F1) and open a debug window. Verify:
  - Debug text/cursor renders without bloom halo.
  - Terrain and mechs still have bloom.
  - No console SPEW about IsHUD leaks.

- [ ] **Step 5: Commit**

  ```bash
  git add code/mechcmd2.cpp
  git commit -m "feat: wrap screen-space HUD callsites in mechcmd2.cpp with gos_State_IsHUD"
  ```

---

## Task 9: Wrap Logistics and Options Screen Renders

**Files:**
- Modify: `code/mechcmd2.cpp`

These screens render only when `mission` is not active — no scene content in the same frame. Function-level wrapping is correct after audit.

- [ ] **Step 1: Read and audit `logistics->render()` and `optionsScreenWrapper->render()`**

  In `mechcmd2.cpp`, find `UpdateRenderers()` around lines 729-746:
  ```cpp
  		if (logistics)
  		{
  			...
  			logistics->render();
  		}

  		if (optionsScreenWrapper && !optionsScreenWrapper->isDone() )
  		{
  			...
  			optionsScreenWrapper->render();
  		}
  ```

  Confirm neither calls `mcTextureManager->renderLists()` or `Camera::UpdateRenderers()` inside. If either does, downgrade to block-level wrapping around only the pure UI submits inside those functions. If both are pure UI, proceed to Step 2.

- [ ] **Step 2: Add `IsHUD` wrapping to logistics and options renders**

  Change:
  ```cpp
  		if (logistics)
  		{
  			ZoneScopedN("UpdateRenderers logisticsRender");
  			float viewMulX, viewMulY, viewAddX, viewAddY;
  			gos_GetViewport(&viewMulX, &viewMulY, &viewAddX, &viewAddY);
  			userInput->setViewport(viewMulX,viewMulY,viewAddX,viewAddY);

  			logistics->render();
  		}

  		if (optionsScreenWrapper && !optionsScreenWrapper->isDone() )
  		{
  			ZoneScopedN("UpdateRenderers optionsRender");
  			float viewMulX, viewMulY, viewAddX, viewAddY;
  			gos_GetViewport(&viewMulX, &viewMulY, &viewAddX, &viewAddY);
  			userInput->setViewport(viewMulX,viewMulY,viewAddX,viewAddY);

  			optionsScreenWrapper->render();
  		}
  ```
  to:
  ```cpp
  		if (logistics)
  		{
  			ZoneScopedN("UpdateRenderers logisticsRender");
  			float viewMulX, viewMulY, viewAddX, viewAddY;
  			gos_GetViewport(&viewMulX, &viewMulY, &viewAddX, &viewAddY);
  			userInput->setViewport(viewMulX,viewMulY,viewAddX,viewAddY);

  			gos_SetRenderState( gos_State_IsHUD, 1 );
  			logistics->render();
  			gos_SetRenderState( gos_State_IsHUD, 0 );
  		}

  		if (optionsScreenWrapper && !optionsScreenWrapper->isDone() )
  		{
  			ZoneScopedN("UpdateRenderers optionsRender");
  			float viewMulX, viewMulY, viewAddX, viewAddY;
  			gos_GetViewport(&viewMulX, &viewMulY, &viewAddX, &viewAddY);
  			userInput->setViewport(viewMulX,viewMulY,viewAddX,viewAddY);

  			gos_SetRenderState( gos_State_IsHUD, 1 );
  			optionsScreenWrapper->render();
  			gos_SetRenderState( gos_State_IsHUD, 0 );
  		}
  ```

- [ ] **Step 3: Build, deploy, verify logistics/options screens render correctly**

  ```bash
  "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" \
    --build build64 --config RelWithDebInfo 2>&1 | tail -20
  cp -f build64/RelWithDebInfo/mc2.exe "A:/Games/mc2-opengl/mc2-win64-v0.1.1/mc2.exe"
  ```
  Navigate to the logistics screen. Verify UI renders without bloom halos. No console SPEW.

- [ ] **Step 4: Commit**

  ```bash
  git add code/mechcmd2.cpp
  git commit -m "feat: wrap logistics and options screen renders with gos_State_IsHUD"
  ```

---

## Task 10: Callsite Discovery and Wrapping — Mission Markers and Nav Beacons

**Files:**
- Modify: `code/missiongui.cpp`, `code/objective.cpp`, `code/mechicon.cpp`, `code/controlgui.cpp`, `code/gametacmap.cpp`

This task requires reading each draw callsite, classifying it (HUD vs scene), and adding the narrowest possible `IsHUD` scope. **Do not wrap entire render functions** unless you have verified they emit no scene content.

- [ ] **Step 1: Find all `gos_DrawQuads`/`gos_DrawLines`/`gos_TextDraw` calls in mission-related files**

  ```bash
  grep -n "gos_DrawQuads\|gos_DrawLines\|gos_DrawTris\|gos_TextDraw\|gos_RenderIndexedArray" \
    code/missiongui.cpp code/objective.cpp code/mechicon.cpp \
    code/controlgui.cpp code/gametacmap.cpp
  ```
  For each line number, read 20 lines of context to classify the draw:
  - Pixel-space x/y with z=0 and rhw > 0 → screen-space HUD → wrap with `IsHUD=1`
  - 3D world-space vertices → scene → no wrapping
  - Text draws in game UI panels → HUD → wrap

- [ ] **Step 2: Wrap mission selection/drag box in `missiongui.cpp`**

  At `missiongui.cpp` line 2961, `gos_DrawQuads(vertices, 4)` is preceded by vertex setup with `z=0.0, rhw=0.5` (pixel-space screen coordinates). This is a selection rectangle — screen-space HUD. Read the enclosing block (starting from where `vertices[0]` is set up) and wrap the entire contiguous draw block:

  ```cpp
  				gos_SetRenderState( gos_State_IsHUD, 1 );
  				gos_DrawQuads(vertices,4);

  				vertices[0].argb	= SB_BLACK;
  				// ... argb resets ...
  				gos_DrawLines(&vertices[0],2);
  				gos_DrawLines(&vertices[1],2);
  				gos_DrawLines(&vertices[2],2);
  				gos_DrawLines(&vertices[3],2);
  				gos_SetRenderState( gos_State_IsHUD, 0 );
  ```

- [ ] **Step 3: Wrap objective marker text in `objective.cpp`**

  At `objective.cpp` around line 2300, `drawShadowText(...)` draws marker text to screen coordinates. Read the enclosing block in `CObjective::render()` to confirm it is screen-space only, then wrap:

  ```cpp
  				gos_SetRenderState( gos_State_IsHUD, 1 );
  				drawShadowText( 0xffffffff, 0xff000000, s_markerFont->getTempHandle(),
  				                pos.x - width/2, pos.y - height/2, true, m_markerText,
  				                0, s_markerFont->getSize(), -2, 2 );
  				gos_SetRenderState( gos_State_IsHUD, 0 );
  ```

- [ ] **Step 4: Wrap force-group icon and pilot icon draws in `mechicon.cpp`**

  At `mechicon.cpp` around line 979-982 and 1043-1044, `gos_DrawQuads` draws pilot video texture and unit icons to pixel-space coordinates. These are HUD elements (force-group panel). Wrap each contiguous draw block narrowly:

  ```cpp
  			gos_SetRenderState( gos_State_IsHUD, 1 );
  			gos_SetRenderState( gos_State_Texture, pilotVideoTexture );
  			// ... vertex setup ...
  			gos_DrawQuads( v, 4 );
  			gos_SetRenderState( gos_State_IsHUD, 0 );
  ```

  And for `renderUnitIcon()` (around line 1043):
  ```cpp
  	gos_SetRenderState( gos_State_IsHUD, 1 );
  	gos_SetRenderState( gos_State_Texture, s_textureHandle[texIndex] );
  	gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_AlphaInvAlpha);
  	gos_SetRenderState( gos_State_Filter, gos_FilterNone);
  	gos_SetRenderState( gos_State_AlphaTest, true);
  	// ... draw call(s) ...
  	gos_SetRenderState( gos_State_IsHUD, 0 );
  ```

  Read the full `renderUnitIcon()` body before editing to confirm it contains only screen-space draws.

- [ ] **Step 5: Audit `controlgui.cpp` and `gametacmap.cpp`**

  ```bash
  grep -n "gos_DrawQuads\|gos_DrawLines\|gos_TextDraw" code/controlgui.cpp code/gametacmap.cpp
  ```
  For each callsite, read context. If pixel-space HUD, wrap with `IsHUD=1/0`. If tactical map markers are world-projected, classify carefully — only add `IsHUD` if the draw should not receive post-processing.

- [ ] **Step 6: Build**

  ```bash
  "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" \
    --build build64 --config RelWithDebInfo 2>&1 | tail -20
  ```

- [ ] **Step 7: Commit**

  ```bash
  git add code/missiongui.cpp code/objective.cpp code/mechicon.cpp \
          code/controlgui.cpp code/gametacmap.cpp
  git commit -m "feat: add gos_State_IsHUD to mission markers, nav beacons, and HUD icon callsites"
  ```

---

## Task 11: Full Build, Deploy, and Visual Validation

**Files:** none — validation only.

- [ ] **Step 1: Build RelWithDebInfo and deploy**

  ```bash
  cd A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev
  "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" \
    --build build64 --config RelWithDebInfo 2>&1 | tail -20
  ```
  Then use the `/mc2-deploy` skill or:
  ```bash
  cp -f build64/RelWithDebInfo/mc2.exe "A:/Games/mc2-opengl/mc2-win64-v0.1.1/mc2.exe"
  diff -q shaders/gos_terrain.frag "A:/Games/mc2-opengl/mc2-win64-v0.1.1/shaders/gos_terrain.frag" || \
    cp -f shaders/gos_terrain.frag "A:/Games/mc2-opengl/mc2-win64-v0.1.1/shaders/gos_terrain.frag"
  ```

- [ ] **Step 2: HUD post-process check (primary validation)**

  Enable bloom (RAlt+F1) and load a mission. Verify:
  - **HUD elements** (health bars, command interface, action buttons, force-group icons, cursor, debug text) render cleanly without bloom halos.
  - **Mission markers and nav beacons** no longer receive scene post-processing or shadow darkening.
  - **Terrain and mechs** still have bloom and shadow post-processing.

- [ ] **Step 3: Scene-pass regression check**

  With bloom enabled, verify:
  - Cement/road overlays still receive the same post-process treatment as before (bloom, shadow).
  - Water still has post-processing.
  - Craters still have post-processing.
  - If any world overlay lost post-processing, it was accidentally wrapped with `IsHUD=1` — find and fix the callsite.

- [ ] **Step 4: Check console for IsHUD hygiene warnings**

  Run for 60+ seconds. Verify no `[HUD] gos_State_IsHUD still set at frame start` SPEW lines appear. If any appear, find the callsite that forgot `gos_SetRenderState(gos_State_IsHUD, 0)` and fix it.

- [ ] **Step 5: Commit validation results and update CLAUDE.md known issues**

  In `CLAUDE.md`, find the known issues section and remove or update the line:
  ```
  - Post-processing (bloom, FXAA) applies to HUD -- needs scene/HUD split
  ```
  Change to:
  ```
  - Post-processing (bloom, FXAA) applies to HUD -- FIXED (gos_State_IsHUD buffering, Apr 2026)
  ```

  ```bash
  git add CLAUDE.md
  git commit -m "docs: mark HUD/scene split as fixed in known issues"
  ```

---

## Self-Review Notes

- **Spec coverage checked:** all five design sections map to tasks. State snapshot contract → Tasks 2–6. Frame loop → Task 7. Game-side callsites → Tasks 8–10. Validation → Task 11.
- **Late submission behavior:** warn + discard implemented in Tasks 4 and 5 via `hudFlushed_` check.
- **Text path (T2):** layout runs at buffer time in Task 5; replay in Task 6 via `replayTextQuads()`.
- **projection_ captured per draw:** done in Tasks 4 and 5. Replayed correctly via `projection_ = call.projection` in `flushHUDBatch()` before each case.
- **State restore:** `priorState`/`priorProjection` snapshot in `flushHUDBatch()` restores renderer state after replay.
- **No heuristic classification:** every `IsHUD=1` is explicit game-code opt-in.

## Known Minor Deviations (accepted)

- `replayTextQuads()` bypasses `beforeDrawCall()`/`afterDrawCall()` so `num_draw_calls_` and `break_draw_call_num_` debug hooks are not bumped on replayed text. Quad/line replay still hits those hooks via real `drawQuads()`/`drawLines()`. Accepted — affects only debug counters.
- `hudBatch_.clear()` per frame frees `std::vector<gos_VERTEX>` storage inside each `HudDrawCall`. If this shows up as heap churn in Tracy, follow-up optimization is a shared pool with `(offset, count)` ranges per call. Not blocking.
