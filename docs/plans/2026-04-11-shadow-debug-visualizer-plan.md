# Shadow Debug Visualizer Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add a screen-corner shadow map visualizer and terrain draw killswitch for debugging the dynamic shadow frustum centering.

**Architecture:** New `shadow_debug.frag` shader renders the shadow depth texture as a diagnostic overlay (magenta=nothing, grayscale=depth, red=near-clip). Rendered via the existing fullscreen quad VAO with a viewport trick to place it in the top-left corner. Terrain killswitch gates the tessellation draw path. Both toggled via RAlt+F-key hotkeys in `gameosmain.cpp`.

**Tech Stack:** OpenGL 4.2, GLSL 420, SDL2 key events

**Working directory:** `A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/`

**Critical rules:**
- Build: ALWAYS `--config RelWithDebInfo`
- Shader files: NO `#version` line — pass `"#version 420\n"` as prefix to `makeProgram()`
- Uniform API: `setFloat`/`setInt` BEFORE `apply()`, not after
- Deploy: NEVER `cp -r`, ALWAYS `cp -f` per file + `diff -q`

---

### Task 1: Create shadow_debug.frag shader

**Files:**
- Create: `shaders/shadow_debug.frag`

**Step 1: Write the shader**

Create `shaders/shadow_debug.frag` with this content:

```glsl
//#version 420 (version provided by prefix)

uniform sampler2D shadowDebugMap;

in vec2 TexCoord;
out vec4 FragColor;

void main()
{
    float d = texture(shadowDebugMap, TexCoord).r;

    // Magenta = depth 1.0 (cleared, nothing written)
    // This is the most important diagnostic: if the whole quad is magenta,
    // the shadow pass wrote nothing.
    if (d >= 0.999) {
        FragColor = vec4(1.0, 0.0, 1.0, 1.0);  // magenta
        return;
    }

    // Red = depth 0.0 (near plane clipping)
    if (d <= 0.001) {
        FragColor = vec4(1.0, 0.0, 0.0, 1.0);  // red
        return;
    }

    // Grayscale ramp for normal depth values
    // Remap to make mid-range values more visible (gamma-like)
    float v = pow(d, 0.5);  // brighten dark areas
    FragColor = vec4(v, v, v, 1.0);
}
```

**Step 2: Commit**

```bash
git add shaders/shadow_debug.frag
git commit -m "feat: add shadow debug visualization shader"
```

---

### Task 2: Add debug overlay state and method to gosPostProcess

**Files:**
- Modify: `GameOS/gameos/gos_postprocess.h` — add members and method declaration
- Modify: `GameOS/gameos/gos_postprocess.cpp` — constructor init, shader compile, destroy, implement overlay

**Step 1: Add members to gos_postprocess.h**

In the public section (after `shadowsEnabled_` on line 35), add:

```cpp
    // Shadow debug overlay
    bool showShadowDebug_;        // master toggle for debug overlay
    int shadowDebugMode_;         // 0=static, 1=dynamic
    void drawShadowDebugOverlay();
```

In the private section (after `dynamicLightSpaceMatrix_` on line 114), add:

```cpp
    glsl_program* shadowDebugProg_;
```

**Step 2: Initialize new members in constructor**

In `gos_postprocess.cpp` constructor (after `dynShadowMapSize_(1024)` on line 61), add to the initializer list:

```cpp
    , shadowDebugProg_(nullptr)
```

And in the constructor body (after `memset(savedViewport_...)`), add:

```cpp
    showShadowDebug_ = false;
    shadowDebugMode_ = 0;
```

**Step 3: Compile debug shader in init()**

In `gos_postprocess.cpp::init()`, after the bloom shader compilation (after line 113), add:

```cpp
    shadowDebugProg_ = glsl_program::makeProgram("shadow_debug",
        "shaders/postprocess.vert", "shaders/shadow_debug.frag", kShaderPrefix);
    if (!shadowDebugProg_ || !shadowDebugProg_->is_valid())
        fprintf(stderr, "gosPostProcess: failed to compile shadow_debug shader\n");
```

**Step 4: Destroy debug shader in destroy()**

In `gos_postprocess.cpp::destroy()`, after the bloom shader cleanup (after line 146), add:

```cpp
    if (shadowDebugProg_) {
        glsl_program::deleteProgram("shadow_debug");
        shadowDebugProg_ = nullptr;
    }
```

**Step 5: Implement drawShadowDebugOverlay()**

Add this method to `gos_postprocess.cpp`, after `endScene()` (after line 399):

```cpp
void gosPostProcess::drawShadowDebugOverlay()
{
    if (!showShadowDebug_ || !shadowDebugProg_ || !shadowDebugProg_->is_valid())
        return;
    if (!initialized_)
        return;

    // Select which shadow texture to visualize
    GLuint tex = (shadowDebugMode_ == 0) ? shadowDepthTex_ : dynShadowDepthTex_;
    if (!tex)
        return;

    // Save current viewport
    GLint prevViewport[4];
    glGetIntegerv(GL_VIEWPORT, prevViewport);

    // Set viewport to top-left corner: 256x256, 16px margin from top-left
    int quadSize = 256;
    int margin = 16;
    glViewport(margin, height_ - quadSize - margin, quadSize, quadSize);

    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);

    // Temporarily switch shadow texture from comparison mode to raw depth read
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);

    // Bind shader and draw
    shadowDebugProg_->setInt("shadowDebugMap", 0);
    shadowDebugProg_->apply();

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);

    glBindVertexArray(quadVAO_);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);

    // CRITICAL: restore comparison mode so PCF sampling works for the next frame
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);

    // Restore viewport and state
    glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
}
```

**Step 6: Call overlay from endScene()**

In `gos_postprocess.cpp::endScene()`, just before the final state restore block (before line 397 `glDepthMask(GL_TRUE);`), add:

```cpp
    // Shadow debug overlay (draws on top of composite)
    drawShadowDebugOverlay();
```

**Step 7: Commit**

```bash
git add GameOS/gameos/gos_postprocess.h GameOS/gameos/gos_postprocess.cpp
git commit -m "feat: add shadow debug overlay rendering in post-process"
```

---

### Task 3: Add terrain draw killswitch to renderer

**Files:**
- Modify: `GameOS/gameos/gameos_graphics.cpp` — add flag + API functions
- Modify: `GameOS/include/gameos.hpp` — declare API functions

**Step 1: Add terrain draw flag to gosRenderer**

In `gameos_graphics.cpp`, in the gosRenderer class private members (near the other terrain flags like `terrain_shadow_softness_`), add:

```cpp
        bool terrain_draw_enabled_ = true;
```

Add public accessors (near `setTerrainShadowSoftness`/`getTerrainShadowSoftness` around line 1286):

```cpp
        void setTerrainDrawEnabled(bool e) { terrain_draw_enabled_ = e; }
        bool getTerrainDrawEnabled() const { return terrain_draw_enabled_; }
```

**Step 2: Gate terrain draw with the flag**

In `gameos_graphics.cpp`, in the `endFrame()` method, find the terrain draw block (line ~2623):

```cpp
    if (curStates_[gos_State_Terrain] && terrain_material_ && terrain_batch_extras_count_ > 0) {
```

Change to:

```cpp
    if (curStates_[gos_State_Terrain] && terrain_material_ && terrain_batch_extras_count_ > 0 && terrain_draw_enabled_) {
```

**Step 3: Add API functions**

At the bottom of `gameos_graphics.cpp` (near the other terrain API functions like `gos_SetTerrainShadowSoftness`), add:

```cpp
void gos_SetTerrainDrawEnabled(bool e) {
    if (g_gos_renderer) g_gos_renderer->setTerrainDrawEnabled(e);
}
bool gos_GetTerrainDrawEnabled() {
    return g_gos_renderer ? g_gos_renderer->getTerrainDrawEnabled() : true;
}
```

**Step 4: Declare in gameos.hpp**

In `gameos.hpp`, near the other terrain API declarations (around line 2314-2315), add:

```cpp
void gos_SetTerrainDrawEnabled(bool e);
bool gos_GetTerrainDrawEnabled();
```

**Step 5: Commit**

```bash
git add GameOS/gameos/gameos_graphics.cpp GameOS/include/gameos.hpp
git commit -m "feat: add runtime terrain draw killswitch"
```

---

### Task 4: Add hotkeys for debug toggles

**Files:**
- Modify: `GameOS/gameos/gameosmain.cpp` — repurpose RAlt+F2 and RAlt+F5 key handlers

**Step 1: Replace tonemapping toggle with shadow debug**

In `gameosmain.cpp::handle_key_down()`, replace the SDLK_F2/tonemapping case (lines 59-66) with:

```cpp
        case SDLK_F2:
            if (keysym->mod & KMOD_RALT) {
                gosPostProcess* pp = getGosPostProcess();
                if (pp) {
                    if (!pp->showShadowDebug_) {
                        pp->showShadowDebug_ = true;
                        pp->shadowDebugMode_ = 0;
                        fprintf(stderr, "Shadow Debug: STATIC map\n");
                    } else if (pp->shadowDebugMode_ == 0) {
                        pp->shadowDebugMode_ = 1;
                        fprintf(stderr, "Shadow Debug: DYNAMIC map\n");
                    } else {
                        pp->showShadowDebug_ = false;
                        fprintf(stderr, "Shadow Debug: OFF\n");
                    }
                }
            }
            break;
```

**Step 2: Replace FXAA toggle with terrain killswitch**

Replace the SDLK_F5/FXAA case (lines 77-85) with:

```cpp
        case SDLK_F5:
            if (keysym->mod & KMOD_RALT) {
                bool cur = gos_GetTerrainDrawEnabled();
                gos_SetTerrainDrawEnabled(!cur);
                fprintf(stderr, "Terrain Draw: %s\n", !cur ? "ON" : "OFF");
            }
            break;
```

**Step 3: Commit**

```bash
git add GameOS/gameos/gameosmain.cpp
git commit -m "feat: RAlt+F2 shadow debug overlay, RAlt+F5 terrain killswitch"
```

---

### Task 5: Build and deploy

**Step 1: Build**

Use `/mc2-build` skill to build the project (RelWithDebInfo).

**Step 2: Deploy**

Use `/mc2-deploy` skill to deploy exe + shaders with diff verification.

**Step 3: Verify shader deployed**

Check that `shadow_debug.frag` was copied to the deploy directory.

**Step 4: Commit any build/deploy fixes if needed**

---

### Task 6: Quick verification checklist

After launching the game:

1. **RAlt+F4** — should show a 256x256 quad in top-left corner showing static shadow map
   - If all magenta: static shadow never rendered (check `staticShadowsRendered_`)
   - If grayscale terrain shapes: static shadows working correctly
2. **RAlt+F4** again — switches to dynamic shadow map
   - If all magenta: dynamic shadow pass writing nothing (frustum centering is the bug)
   - If grayscale shapes: dynamic shadows are rendering, frustum issue may be resolved
3. **RAlt+F4** again — overlay disappears
4. **RAlt+F6** — terrain should disappear, leaving only 3D objects visible
5. **Tracy** — check `Shadow.DynPass` and `Shadow.DynMatrixBuild` zones are firing

---

## Hotkey Reference (updated)

| Key | Function |
|-----|----------|
| RAlt+F1 | Toggle bloom |
| RAlt+F2 | Cycle shadow debug: off → static → dynamic → off |
| RAlt+F3 | Toggle shadows |
| RAlt+F5 | Toggle terrain draw |
| F6/F7 | Tess level up/down |
| F8/F10 | Phong alpha up/down |
| F11/F12 | Displacement scale up/down |
| ` (grave) | Tessellation wireframe |
| [ / ] | Shadow softness down/up |
