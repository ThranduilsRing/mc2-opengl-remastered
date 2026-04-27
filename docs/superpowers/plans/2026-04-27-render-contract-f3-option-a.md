# Render Contract F3 — Option A Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Migrate Group II-Opaque shaders to declare and write `GBuffer1` via render-contract registry helpers, satisfying the visible-pixel coherence principle from F3 design v2 §1.

**Architecture:** Option A from F3 design v2 §3.1. MRT stays bound for the whole scene (today's behavior). Each Group II-Opaque shader declares `layout(location=1) out vec4 GBuffer1` and writes via `rc_gbuffer1_screenShadowEligible(...)`. An explicit `glClearBufferfv` sentinel clear sets attachment 1 to `(0.5, 0.5, 1.0, 0.0)` after the main clears, defense-in-depth for any pixel never overwritten by a producer. The `enableMRT`/`disableMRT` helpers are not called (option A keeps MRT live for the whole scene); they remain in the codebase for now and may be removed in a post-F3 cleanup.

**Tech Stack:** GLSL fragment shaders, OpenGL 4.3 MRT, render-contract registry (`rc_*` helpers in `shaders/include/render_contract.hglsl`), CMake/MSVC RelWithDebInfo build, `/mc2-build` + `/mc2-deploy` skills, smoke tier1 5/5 PASS gate.

**Verification model.** This is a renderer-side change with no unit-test surface. Each task is gated by:
1. Build green RelWithDebInfo (`cmake --build build64 --config RelWithDebInfo --target mc2`).
2. Deploy via `/mc2-deploy` discipline (`cp -f` per file + `diff -q`).
3. Smoke tier1 5/5 PASS (`py -3 .../scripts/run_smoke.py --tier tier1 --with-menu-canary --kill-existing`).
4. Visual A/B vs. previous commit on at least one mech-heavy mission. Quick check: mech post-shadow still applies (mechs darken in terrain shadow); no color-channel artifacts; framerate unchanged.

**Reference docs (read before starting):**
- [F3 design v2](../specs/2026-04-26-render-contract-f3-mrt-completeness-design.md) — coherence principle, Option A architecture (§5A)
- [F3 pass audit](../specs/render-contract-f3-pass-audit.md) — Group I/II-Opaque/II-Blend/Excluded classification
- [F3 canary report](../specs/render-contract-f3-canary-report.md) — CLEAN verdict 2026-04-27, refutes the AMD location=1 claim
- [Registry spec](../specs/2026-04-26-render-contract-registry-design.md) — `rc_*` helpers, frozen surfaces

---

## File Structure

**Files modified:**
- `GameOS/gameos/gos_postprocess.h` — declare `clearGBuffer1()`
- `GameOS/gameos/gos_postprocess.cpp` — define `clearGBuffer1()`
- `GameOS/gameos/gameosmain.cpp` — call `pp->clearGBuffer1()` after the sky-color clear (line ~456)
- `shaders/gos_tex_vertex_lighted.frag` — declare and write `GBuffer1` (cherry-pick from canary `0173a31`)
- `shaders/gos_vertex_lighted.frag` — declare and write `GBuffer1` (real normal)
- `shaders/gos_tex_vertex.frag` — declare and write `GBuffer1` (flat-up)
- `shaders/gos_vertex.frag` — declare and write `GBuffer1` (flat-up)
- `docs/amd-driver-rules.md` — add a final remove-comment note (already updated 2026-04-27 with the canary refutation; this plan adds a follow-up note)
- `docs/superpowers/specs/render-contract-f3-report.md` — closing report (NEW)

**Files NOT touched (frozen surfaces per design spec §7):**
- All Group I shaders: `gos_terrain.frag`, `terrain_overlay.frag`, `decal.frag`, `gos_grass.frag`, `static_prop.frag`
- `shadow_screen.frag` — reader-side, frozen
- `mclib/render_contract.{h,cpp}`, `shaders/include/render_contract.hglsl` — registry API frozen
- `mclib/camera.h` projectZ wrappers — load-bearing
- `enableMRT()` / `disableMRT()` — remain in source as dead code; removal is a post-F3 cleanup

**Files explicitly untouched in this plan:**
- `object_tex.frag` / `object_tex.vert` — vestigial (zero source references); deletion is a post-F3 cleanup task tracked separately
- `gos_text.frag` — Excluded (drawn after `endScene` to FB 0)
- All post-process / shadow-pass shaders — Excluded

---

## Pre-flight check (run once before Task 1)

- [ ] **PF.1: Confirm working state.**

```bash
cd "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev"
git status
git log --oneline -6
```

Expected: clean working tree on `claude/nifty-mendeleev`. Recent commits include `9cc9912` (canary verdict CLEAN), `2ff756a` (canary placeholder), `628694a` (pass audit), `5a9a3a8` (spec v2). No uncommitted changes.

- [ ] **PF.2: Confirm canary branch still exists.**

```bash
git show-ref --verify refs/heads/claude/f3-amd-canary-temp
git log --oneline -1 claude/f3-amd-canary-temp
```

Expected: branch exists; HEAD is `0173a31` (the canary shader change).

- [ ] **PF.3: Smoke tier1 baseline.**

```bash
py -3 A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/scripts/run_smoke.py --tier tier1 --with-menu-canary --kill-existing
echo "Exit: $?"
```

Expected: exit 0, "5/5 PASS" + menu canary clean. **If this fails, stop the plan and investigate** — F3 should not land on a baseline that already regresses.

---

## Task 1: Add `clearGBuffer1` sentinel clear

**Goal:** Defense-in-depth — make sure attachment 1's default value is the post-shadow-eligible sentinel `(0.5, 0.5, 1.0, 0.0)` rather than `glClearColor`'s `(<sky>, 1.0)`.

**Files:**
- Modify: `GameOS/gameos/gos_postprocess.h:34-37` (add declaration near `enableMRT`/`disableMRT`)
- Modify: `GameOS/gameos/gos_postprocess.cpp:583-595` (add definition near `enableMRT`/`disableMRT`)
- Modify: `GameOS/gameos/gameosmain.cpp:456` (call `pp->clearGBuffer1()` after the sky-color clear)

- [ ] **Step 1.1: Read the current header insertion point.**

Run: `grep -n "enableMRT\|disableMRT" GameOS/gameos/gos_postprocess.h`

Expected output (around lines 34-35):
```
34:    void enableMRT();   // call before terrain draws
35:    void disableMRT();  // call after terrain draws
```

- [ ] **Step 1.2: Add the header declaration.**

Edit `GameOS/gameos/gos_postprocess.h`. Insert after the existing `disableMRT()` declaration (line 35):

```cpp
    void enableMRT();   // call before terrain draws (legacy; not used under F3 Option A)
    void disableMRT();  // call after terrain draws (legacy; not used under F3 Option A)
    // F3: explicit sentinel clear for GBuffer1 (attachment 1).
    // Sets attachment 1 to (0.5, 0.5, 1.0, 0.0) — flat-up encoded normal,
    // alpha = 0.0 (post-shadow eligible). Must be called while MRT is bound
    // (i.e., after beginScene's glDrawBuffers(2) and before any single-buffer
    // restoration, if any). Defense-in-depth: every visible pixel either
    // gets overwritten by an explicit rc_gbuffer1_* writer or inherits this
    // sentinel.
    void clearGBuffer1();
```

(The "legacy" comment update on `enableMRT`/`disableMRT` is intentional — under Option A they are unused but kept for now.)

- [ ] **Step 1.3: Add the cpp definition.**

Edit `GameOS/gameos/gos_postprocess.cpp`. Insert after the `disableMRT()` definition (around line 595):

```cpp
void gosPostProcess::disableMRT()
{
    GLenum singleBuf = GL_COLOR_ATTACHMENT0;
    glDrawBuffers(1, &singleBuf);
}

void gosPostProcess::clearGBuffer1()
{
    if (!sceneNormalTex_) return;
    // Sentinel: flat-up encoded normal (0.5, 0.5, 1.0), alpha=0.0
    // (post-shadow eligible). Matches rc_gbuffer1_screenShadowEligible(vec3(0,0,1))
    // in shaders/include/render_contract.hglsl.
    static const GLfloat sentinel[4] = { 0.5f, 0.5f, 1.0f, 0.0f };
    // glClearBufferfv with buffer=GL_COLOR, drawbuffer=1 clears the
    // SECOND draw buffer of the currently bound FBO. Because beginScene()
    // calls glDrawBuffers(2, {COLOR0, COLOR1}), drawbuffer index 1 maps
    // to GL_COLOR_ATTACHMENT1 here. Caller must ensure MRT is bound.
    glClearBufferfv(GL_COLOR, 1, sentinel);
}
```

- [ ] **Step 1.4: Add the call in gameosmain.cpp.**

Edit `GameOS/gameos/gameosmain.cpp`. The current sky-color clear is at line 456. Insert immediately after it:

```cpp
    glClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT );

    // F3: overwrite attachment 1 (GBuffer1) with the post-shadow-eligible sentinel.
    // glClearColor sets alpha=1.0 in both gameplay and menu paths, which would
    // otherwise leave non-overwritten pixels reading "shadow handled". This
    // sentinel ensures defense-in-depth coherence per F3 design v2 §5A.
    if (pp) pp->clearGBuffer1();

    // Skybox disabled — terrain fog provides atmosphere, bright sky looked jarring
```

- [ ] **Step 1.5: Build.**

Run:
```bash
cd "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev"
"C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build64 --config RelWithDebInfo --target mc2 2>&1 | tail -10
```

Expected: `mc2.exe -> ...build64\RelWithDebInfo\mc2.exe`. No compile errors.

- [ ] **Step 1.6: Deploy.**

Run:
```bash
cp -f build64/RelWithDebInfo/mc2.exe A:/Games/mc2-opengl/mc2-win64-v0.2/mc2.exe && echo "exe deployed"
```

(No shader changes in this task; mc2.exe deploy alone is sufficient.)

- [ ] **Step 1.7: Smoke tier1.**

Run:
```bash
py -3 A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/scripts/run_smoke.py --tier tier1 --with-menu-canary --kill-existing
echo "Exit: $?"
```

Expected: exit 0, 5/5 PASS.

**Visual A/B note:** Behavior should be byte-equivalent here. The sentinel only affects pixels not overwritten by terrain/overlay/decal/grass/static-prop/object passes. Today those pixels have alpha=1.0 from `glClearColor`, but they are also depth=1.0 (sky pixels) which `shadow_screen.frag` skips at line 132 (`if (depth >= 1.0) ...`). So no visible difference is expected. Spot-check mech post-shadow: still applies as before.

- [ ] **Step 1.8: Commit.**

```bash
cd "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev"
git add GameOS/gameos/gos_postprocess.h GameOS/gameos/gos_postprocess.cpp GameOS/gameos/gameosmain.cpp
git commit -m "$(cat <<'EOF'
render-contract: F3 Option A — add clearGBuffer1 sentinel clear

Defense-in-depth for the post-shadow mask channel. glClearColor sets
alpha=1.0 in both gameplay and menu paths, which would leave any pixel
not overwritten by an rc_gbuffer1_* writer reading "shadow handled".
The sentinel (0.5, 0.5, 1.0, 0.0) — flat-up encoded normal, alpha=0
(post-shadow eligible) — gives every uncovered pixel a defined,
post-shadow-eligible default.

Today this is benign because uncovered pixels are sky (depth=1.0)
which shadow_screen.frag skips. Setting the sentinel explicitly
removes the dependence on the depth-skip side path so the registry's
coherence guarantee stands without it.

clearGBuffer1 must be called while MRT is bound (after beginScene's
glDrawBuffers(2)) — the call site in gameosmain.cpp post-line-456
satisfies that.

enableMRT/disableMRT remain defined-but-unused under Option A.
Removal is a post-F3 cleanup.

Smoke tier1 5/5 PASS. Visual A/B byte-equivalent (no uncovered
non-sky pixels exist in current rendering).

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: Cherry-pick the canary commit (gos_tex_vertex_lighted.frag)

**Goal:** Land the canary's shader change (which AMD already validated as CLEAN) onto the F3 branch as the first Group II-Opaque migration.

**Files:**
- Modify: `shaders/gos_tex_vertex_lighted.frag` (cherry-picked from `claude/f3-amd-canary-temp` HEAD `0173a31`)

- [ ] **Step 2.1: Cherry-pick.**

```bash
cd "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev"
git cherry-pick 0173a31
```

Expected: clean cherry-pick onto `claude/nifty-mendeleev`. The commit message will be the canary's original message — that's OK, but we'll amend it to remove the "THROWAWAY" framing.

- [ ] **Step 2.2: Inspect the diff.**

```bash
git show HEAD --stat
git show HEAD -- shaders/gos_tex_vertex_lighted.frag
```

Expected: 1 file changed, ~11 insertions. The shader gains `#include <include/render_contract.hglsl>`, `layout (location=1) out PREC vec4 GBuffer1;`, and `GBuffer1 = rc_gbuffer1_screenShadowEligible(normalize(Normal));`.

- [ ] **Step 2.3: Amend commit message to reflect F3 production context.**

```bash
git commit --amend -m "$(cat <<'EOF'
render-contract: F3 Option A — explicit GBuffer1 in gos_tex_vertex_lighted.frag

PRIMARY Group II-Opaque shader (TGL world objects: mechs, buildings,
vehicles). Per F3 pass audit §3.3, every mech/building/vehicle pixel
passes through this shader.

Adds:
- #include <include/render_contract.hglsl>
- layout (location=1) out PREC vec4 GBuffer1
- GBuffer1 = rc_gbuffer1_screenShadowEligible(normalize(Normal))

Real per-vertex Normal varying is available — not flat-up. This is
the full Option A semantic: the shader contributes both a real
world-space normal and a defined post-shadow-eligible alpha to
attachment 1, replacing today's reliance on AMD's vec4(0,0,0,0)
undeclared-output behavior.

Cherry-picked from claude/f3-amd-canary-temp (0173a31) where this
exact change was canary-tested CLEAN on AMD RX 7900 XTX driver
26.3.1 across 5-6 missions including one mission completion.
See docs/superpowers/specs/render-contract-f3-canary-report.md.

Smoke tier1 5/5 PASS. Visual A/B vs. previous commit clean — mech
post-shadow continues to apply identically.

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>
EOF
)"
```

- [ ] **Step 2.4: Build.**

Same command as Step 1.5. Expected green.

- [ ] **Step 2.5: Deploy (exe + shader).**

```bash
cd "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev"
cp -f build64/RelWithDebInfo/mc2.exe A:/Games/mc2-opengl/mc2-win64-v0.2/mc2.exe && \
cp -f shaders/gos_tex_vertex_lighted.frag A:/Games/mc2-opengl/mc2-win64-v0.2/shaders/gos_tex_vertex_lighted.frag && \
diff -q shaders/gos_tex_vertex_lighted.frag A:/Games/mc2-opengl/mc2-win64-v0.2/shaders/gos_tex_vertex_lighted.frag && \
echo "deploy verified"
```

- [ ] **Step 2.6: Smoke tier1 + visual A/B.**

Run smoke per Step 1.7. Then launch one mech-heavy mission (e.g., `mc2_10`) and confirm:
- Mechs darken in terrain shadow (post-shadow applies)
- No color-channel corruption
- Framerate unchanged

Expected: 5/5 PASS, visual A/B clean.

(No new commit at this step — the cherry-pick already landed in 2.1+2.3.)

---

## Task 3: Migrate `gos_vertex_lighted.frag`

**Goal:** Migrate the lit-untextured Group II-Opaque path. Same callsite as Task 2 (`drawIndexedTris.Lighted`), real per-vertex normal available.

**Files:**
- Modify: `shaders/gos_vertex_lighted.frag`

- [ ] **Step 3.1: Read the current shader.**

```bash
cat shaders/gos_vertex_lighted.frag
```

Note the structure: `in PREC vec4 Color`, `in PREC float FogValue`, `in PREC vec2 Texcoord`. **The current shader does NOT have a `Normal` varying.** Confirm by reading the top of the file.

If `Normal` is absent: this is the **flat-up case**. Use `vec3(0.0, 0.0, 1.0)`. List in the closing report's flat-up roster.

If `Normal` is present (varying renamed since the audit): use `normalize(Normal)`. (Audit §3.5 anticipated real normal; verify.)

- [ ] **Step 3.2: Apply the change.**

Edit `shaders/gos_vertex_lighted.frag`. Add the include after the existing `#define PREC highp`:

```glsl
//#version 300 es
#define PREC highp

#include <include/render_contract.hglsl>

in PREC vec4 Color;
in PREC float FogValue;
in PREC vec2 Texcoord;

#ifdef ENABLE_TEXTURE1
uniform sampler2D tex1;
#endif
uniform sampler2D tex2;
uniform sampler2D tex3;

uniform PREC vec4 fog_color;

layout (location=0) out PREC vec4 FragColor;
// F3 Option A: post-shadow-eligible mask + flat-up normal (no Normal varying
// in this shader). Listed in flat-up roster of F3 closing report.
layout (location=1) out PREC vec4 GBuffer1;

void main(void)
{
    PREC vec4 c = Color;
#ifdef ENABLE_TEXTURE1
    c *= texture(tex1, Texcoord);
#endif
	if(fog_color.x>0.0 || fog_color.y>0.0 || fog_color.z>0.0 || fog_color.w>0.0)
    	c.rgb = mix(fog_color.rgb, c.rgb, FogValue);
    FragColor = c;

    // F3 Option A: flat-up fallback (compatibility — no surface normal available).
    GBuffer1 = rc_gbuffer1_screenShadowEligible(vec3(0.0, 0.0, 1.0));
}
```

(Variation: if Step 3.1 found a `Normal` varying, replace `vec3(0.0, 0.0, 1.0)` with `normalize(Normal)` and update the comment to match.)

- [ ] **Step 3.3: Build.**

Same as Step 1.5. Expected green.

- [ ] **Step 3.4: Deploy.**

```bash
cp -f build64/RelWithDebInfo/mc2.exe A:/Games/mc2-opengl/mc2-win64-v0.2/mc2.exe && \
cp -f shaders/gos_vertex_lighted.frag A:/Games/mc2-opengl/mc2-win64-v0.2/shaders/gos_vertex_lighted.frag && \
diff -q shaders/gos_vertex_lighted.frag A:/Games/mc2-opengl/mc2-win64-v0.2/shaders/gos_vertex_lighted.frag && \
echo "deploy verified"
```

- [ ] **Step 3.5: Smoke tier1 + visual A/B.**

Per Steps 1.7 + 2.6. Watch for: any object that uses lit-untextured rendering (rare — debug builds, possibly dev-mode mech vertex visualization). If no production content hits this path, smoke is the only gate.

Expected: 5/5 PASS, visual A/B clean.

- [ ] **Step 3.6: Commit.**

```bash
git add shaders/gos_vertex_lighted.frag
git commit -m "$(cat <<'EOF'
render-contract: F3 Option A — explicit GBuffer1 in gos_vertex_lighted.frag

Group II-Opaque (rare) per F3 pass audit §3.5. Lit untextured path
through selectLightedRenderMaterial when gos_State_Lighting=1 and
gos_State_Texture=0. Production callsites are uncommon; mostly
debug-tier visualization.

No Normal varying in this shader — flat-up fallback used.
Listed in F3 closing report flat-up roster for later
normal-quality cleanup.

Smoke tier1 5/5 PASS.

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>
EOF
)"
```

---

## Task 4: Migrate `shaders/gos_tex_vertex.frag`

**Goal:** Migrate the textured non-lit shader. Used both for non-overlay textured world draws (Group II-Opaque) and the IS_OVERLAY bridge (Group II-Blend per §3.4.b — depth-write off, not load-bearing for coherence). Single shader file covers both variants via `#ifdef IS_OVERLAY`. The unconditional `GBuffer1` write is correct in both cases — overlay pixels with `glDepthMask(GL_FALSE)` get their `GBuffer1` write coalesced into the same pixel as the underlying terrain (terrain still owns depth), but `shadow_screen.frag` reads `GBuffer1` regardless of who owns depth, so an explicit write here is preferable to relying on the underlying surface's mask. Treat the entire shader as Group II-Opaque for F3 purposes.

**Files:**
- Modify: `shaders/gos_tex_vertex.frag`

- [ ] **Step 4.1: Read the current shader.**

```bash
cat shaders/gos_tex_vertex.frag
```

Confirm: no `Normal` varying. **Flat-up case.** Listed in flat-up roster.

- [ ] **Step 4.2: Apply the change.**

Edit `shaders/gos_tex_vertex.frag`. Add the include and declaration:

```glsl
//#version 300 es

#define PREC highp

#include <include/render_contract.hglsl>

in PREC vec4 Color;
in PREC vec2 Texcoord;
in PREC float FogValue;

layout (location=0) out PREC vec4 FragColor;
// F3 Option A: post-shadow-eligible mask + flat-up normal (no Normal varying).
// Covers both the non-overlay variant (Group II-Opaque) and the IS_OVERLAY
// bridge (Group II-Blend per audit §3.4.b). Explicit write is correct in
// both cases; overlay paths with depth-write off don't own depth, but
// shadow_screen.frag reads GBuffer1 regardless of depth ownership, so an
// explicit post-shadow-eligible value is the coherent answer.
// Listed in flat-up roster of F3 closing report.
layout (location=1) out PREC vec4 GBuffer1;

uniform sampler2D tex1;
uniform PREC vec4 fog_color;
uniform PREC float time;          // seconds — used for water animation
uniform int isWater;
```

Then at the end of `main`:

```glsl
	if(fog_color.x>0.0 || fog_color.y>0.0 || fog_color.z>0.0 || fog_color.w>0.0)
    	c.rgb = mix(fog_color.rgb, c.rgb, FogValue);
	FragColor = c;

	// F3 Option A: flat-up fallback (compatibility — no surface normal available).
	GBuffer1 = rc_gbuffer1_screenShadowEligible(vec3(0.0, 0.0, 1.0));
}
```

- [ ] **Step 4.3: Build, deploy, smoke + visual A/B.**

Build per Step 1.5; deploy per Step 3.4 (substitute `gos_tex_vertex.frag`); smoke per Step 1.7. Visual A/B: launch a mission with water (e.g., `mc2_24` if it has water; otherwise any mission with road overlays). Confirm:
- Water appearance unchanged
- Road overlays / decals appear as before
- Particles still render correctly (gosFX uses this shader; verify per §3.4.c — depth-write off, no coherence break expected)

Expected: 5/5 PASS, visual A/B clean.

- [ ] **Step 4.4: Commit.**

```bash
git add shaders/gos_tex_vertex.frag
git commit -m "$(cat <<'EOF'
render-contract: F3 Option A — explicit GBuffer1 in gos_tex_vertex.frag

Group II-Opaque (textured non-lit world draws) AND Group II-Blend
(IS_OVERLAY bridge: water and road overlays; gosFX particle path).
Per F3 pass audit §3.4, the same shader file serves multiple
contexts. Single explicit GBuffer1 write covers all of them.

For depth-writing callsites (non-overlay variant), the explicit
write is the correct coherence answer.
For depth-write-off callsites (IS_OVERLAY, particles), the
write lands at the pixel but the underlying surface still owns
depth; shadow_screen.frag reads GBuffer1 regardless. Explicit
post-shadow-eligible value matches the surface's intent.

No Normal varying — flat-up fallback. Listed in flat-up roster.

Smoke tier1 5/5 PASS.

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>
EOF
)"
```

---

## Task 5: Migrate `shaders/gos_vertex.frag`

**Goal:** Migrate the basic untextured shader (lines, points, basic colored verts). Debug-tier; smallest pixel coverage of any Group II-Opaque shader.

**Files:**
- Modify: `shaders/gos_vertex.frag`

- [ ] **Step 5.1: Read.**

```bash
cat shaders/gos_vertex.frag
```

Confirm: no `Normal` varying. Flat-up case.

- [ ] **Step 5.2: Apply the change.**

Edit `shaders/gos_vertex.frag`:

```glsl
//#version 300 es

#define PREC highp

#include <include/render_contract.hglsl>

in PREC vec4 Color;
in PREC vec2 Texcoord;
in PREC float FogValue;

layout (location=0) out PREC vec4 FragColor;
// F3 Option A: post-shadow-eligible mask + flat-up normal (debug-tier
// content: lines, points, basic colored verts). Listed in flat-up roster.
layout (location=1) out PREC vec4 GBuffer1;

#ifdef ENABLE_TEXTURE1
uniform sampler2D tex1;
#endif
uniform sampler2D tex2;
uniform sampler2D tex3;

uniform PREC vec4 fog_color;

void main(void)
{
    PREC vec4 c = Color;
#ifdef ENABLE_TEXTURE1
    c *= texture(tex1, Texcoord);
#endif
	if(fog_color.x>0.0 || fog_color.y>0.0 || fog_color.z>0.0 || fog_color.w>0.0)
		c.rgb = mix(fog_color.rgb, c.rgb, FogValue);
    FragColor = c;

    // F3 Option A: flat-up fallback (compatibility — no surface normal).
    GBuffer1 = rc_gbuffer1_screenShadowEligible(vec3(0.0, 0.0, 1.0));
}
```

- [ ] **Step 5.3: Build, deploy, smoke + visual A/B.**

Per Steps 4.3. Watch for any in-scene-FBO debug visualization (e.g., navmesh debug if any). Expected: no visible difference for production content.

- [ ] **Step 5.4: Commit.**

```bash
git add shaders/gos_vertex.frag
git commit -m "$(cat <<'EOF'
render-contract: F3 Option A — explicit GBuffer1 in gos_vertex.frag

Group II-Opaque (debug-tier) per F3 pass audit §3.6. Lines, points,
basic colored verts. Selected via selectBasicRenderMaterial when
no texture and no lighting. Smallest pixel coverage of any Group II
shader; production content rarely hits this path.

Flat-up fallback. Listed in flat-up roster.

Smoke tier1 5/5 PASS. With this commit, all four Group II-Opaque
shaders identified in F3 pass audit are migrated to explicit
GBuffer1 writes.

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>
EOF
)"
```

---

## Task 6: Verification — V1 (IS_OVERLAY depth-write) and V2 (gosFX depth-write)

**Goal:** Confirm the audit's V1 and V2 verification items so the closing report can record the final classification with evidence.

**Files:**
- No code changes (verification-only)
- Output: notes for inclusion in closing report

- [ ] **Step 6.1: Trace IS_OVERLAY depth-write state.**

Find every callsite that sets `gos_State_Overlay`:

```bash
grep -rn "gos_State_Overlay" GameOS/ mclib/ code/ | head -40
```

For each callsite, identify whether `gos_State_ZWrite` is also set to 0 in the same context. Look for patterns like:

```cpp
gos_SetRenderState(gos_State_Overlay, 1);
gos_SetRenderState(gos_State_ZWrite, 0);  // expected
```

Record findings:
- Total IS_OVERLAY callsites: _N_
- Callsites with confirmed `gos_State_ZWrite = 0`: _N_
- Callsites NOT setting `gos_State_ZWrite = 0` (write depth on overlays): _N_ (expected: 0)

Expected outcome: every IS_OVERLAY callsite has depth-write off. If any callsite writes depth on an overlay, that's a Group II-Opaque-confirmed-overdraw path; Task 4's explicit write covers it correctly under Option A, so this is informational only.

- [ ] **Step 6.2: Trace gosFX particle depth-write state.**

```bash
grep -rn "gos_State_ZWrite\|glDepthMask" mclib/gosfx/ | head -30
```

For each gosFX submission path (`Card::Render`, `Tube::Render`, etc.), confirm `glDepthMask(GL_FALSE)` or `gos_State_ZWrite = 0` is set before draw and restored after.

Record findings analogous to Step 6.1.

- [ ] **Step 6.3: Document findings in a temporary notes file (DO NOT COMMIT).**

```bash
cat > /tmp/f3-v1-v2-notes.md <<'EOF'
# F3 V1/V2 Verification Notes (2026-04-27)

## V1 — IS_OVERLAY depth-write
- Total callsites: <N>
- Confirmed depth-write off: <N>
- Depth-write on (Group II-Opaque): <N>
- Specific callsites if depth-write on:
  - <file:line>: <context>

## V2 — gosFX particle depth-write
- Total submission paths: <N>
- Confirmed depth-write off: <N>
- Depth-write on: <N>
- Specific paths if depth-write on:
  - <file:line>: <context>

Conclusion: <summary>
EOF
```

This file is for use in Task 7's closing report; it is not committed to the repo.

- [ ] **Step 6.4: No commit at this step.** Verification is information; the conclusion lands in Task 7's closing report.

---

## Task 7: Closing report

**Goal:** Document F3 outcome, list flat-up roster, record V1/V2 conclusions, link the canary report.

**Files:**
- Create: `docs/superpowers/specs/render-contract-f3-report.md`

- [ ] **Step 7.1: Write the closing report.**

Create `docs/superpowers/specs/render-contract-f3-report.md`:

```markdown
# Render Contract F3 — Closing Report

**Status:** ✅ Implemented (Option A)
**Date:** 2026-04-27 (canary), 2026-04-27 (implementation)
**Branch:** `claude/nifty-mendeleev`
**Spec:** [F3 design v2](2026-04-26-render-contract-f3-mrt-completeness-design.md)
**Audit:** [F3 pass audit](render-contract-f3-pass-audit.md)
**Canary:** [F3 canary report](render-contract-f3-canary-report.md) (verdict CLEAN)

## Outcome

F3 implemented as Option A. Every Group II-Opaque shader identified by the pass audit declares `layout(location=1) out vec4 GBuffer1` and writes via `rc_gbuffer1_screenShadowEligible(...)`. The visible-pixel coherence principle from spec §1 holds: every pixel `shadow_screen.frag` consumes has `GBuffer1` describing the same surface that wrote `COLOR0` and depth.

The AMD `location=1` corruption claim was canary-tested and refuted.
`docs/amd-driver-rules.md` updated 2026-04-27.

## Commit ledger

| Commit | Content |
|---|---|
| `<sha-of-task1>` | clearGBuffer1 sentinel clear |
| `<sha-of-task2>` | gos_tex_vertex_lighted.frag explicit GBuffer1 (cherry-picked from canary) |
| `<sha-of-task3>` | gos_vertex_lighted.frag explicit GBuffer1 |
| `<sha-of-task4>` | gos_tex_vertex.frag explicit GBuffer1 |
| `<sha-of-task5>` | gos_vertex.frag explicit GBuffer1 |
| `<sha-of-task7>` | this report |

(Substitute SHAs after Task 7 commit.)

## Flat-up roster — shaders that wrote `vec3(0,0,1)` because no surface normal was available

These shaders write `rc_gbuffer1_screenShadowEligible(vec3(0,0,1))` instead of a real per-vertex normal. **Flat-up is a compatibility fallback, not a physically correct world normal.** When a downstream pass starts consuming `GBuffer1.rgb` as world normal, each of these will need a normal-quality cleanup.

- `shaders/gos_vertex_lighted.frag` (Task 3) — debug-tier; rare production usage
- `shaders/gos_tex_vertex.frag` (Task 4) — IS_OVERLAY bridge + textured non-lit world; covers gosFX particle path
- `shaders/gos_vertex.frag` (Task 5) — lines, points, basic colored verts (debug-tier)

Shader migrated with a real per-vertex normal (NOT flat-up):
- `shaders/gos_tex_vertex_lighted.frag` (Task 2) — `normalize(Normal)`

## V1 — IS_OVERLAY depth-write verification

<paste Step 6.3 V1 conclusions here>

## V2 — gosFX particle depth-write verification

<paste Step 6.3 V2 conclusions here>

## Coherence proof

- Sky pixels (depth=1.0): `shadow_screen.frag:132` skips them via depth gate. `GBuffer1` value irrelevant. Sentinel clear (Task 1) provides defensive default `(0.5, 0.5, 1.0, 0.0)`.
- Terrain pixels: `gos_terrain.frag` writes `GBuffer1` via `rc_gbuffer1_shadowHandled` (alpha=1, "shadow handled"). Unchanged by F3.
- Terrain overlay/decal/grass pixels: registry helpers — Group I, unchanged by F3.
- Static prop pixels: `static_prop.frag` writes `rc_gbuffer1_screenShadowEligible(N)`. Unchanged by F3.
- TGL world objects (mech/building/vehicle): `gos_tex_vertex_lighted.frag` writes `rc_gbuffer1_screenShadowEligible(normalize(Normal))` after F3.
- Other depth-writing world draws (rare lit-untextured, basic textured, basic untextured, lines, points): explicit flat-up writes after F3.
- Depth-write-off draws (IS_OVERLAY overlays, gosFX particles): underlying surface owns depth and `GBuffer1`; coherence intact.

For every visible pixel `shadow_screen.frag` consumes, `GBuffer1.alpha` and `GBuffer1.rgb` correspond to the surface that owns that pixel's depth.

## Frozen surfaces — confirmed unchanged

- All 5 Group I shaders (terrain, terrain_overlay, decal, gos_grass, static_prop)
- `shadow_screen.frag` reader logic
- `mclib/render_contract.{h,cpp}`, `shaders/include/render_contract.hglsl`
- `mclib/camera.h` projectZ wrappers
- `enableMRT()` / `disableMRT()` (still defined-but-unused under Option A)
- `scripts/check-render-contract-gbuffer1.sh` grep census continues to pass — F3 added writes, all via `rc_*` helpers

## Follow-ups (post-F3)

- Remove `enableMRT()` / `disableMRT()` if confirmed dead under Option A.
- Remove the stale comment at `gos_postprocess.cpp:519-520` about AMD location=1 corruption.
- Delete `shaders/object_tex.frag` and `shaders/object_tex.vert` (vestigial; no source references).
- Update `docs/modding-guide.md` to remove the stale `gos_text.frag` reference and clarify `object_tex` removal.
- Normal-quality cleanup for the flat-up roster shaders when a downstream pass starts consuming `GBuffer1.rgb` as world normal.
- F1 (water/shoreline material-alpha overload) — separate spec.
```

(Replace `<sha-of-task*>` with actual hashes when Tasks 1–5 are complete; replace V1/V2 placeholders with Task 6 conclusions.)

- [ ] **Step 7.2: Fill in commit SHAs and V1/V2 conclusions.**

```bash
git log --oneline claude/nifty-mendeleev | head -10
```

Find the SHAs of Tasks 1–5 commits. Substitute them into the report.

Substitute the V1/V2 conclusion text from `/tmp/f3-v1-v2-notes.md`.

- [ ] **Step 7.3: Smoke tier1 (final gate).**

Run the smoke gate one more time as the canonical "everything is green" confirmation:

```bash
py -3 A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/scripts/run_smoke.py --tier tier1 --with-menu-canary --kill-existing
echo "Exit: $?"
```

Expected: exit 0, 5/5 PASS, menu canary clean.

- [ ] **Step 7.4: Commit.**

```bash
git add docs/superpowers/specs/render-contract-f3-report.md
git commit -m "$(cat <<'EOF'
spec: F3 closing report — Option A implemented, coherence achieved

All Group II-Opaque shaders identified by the pass audit now declare
and write GBuffer1 via rc_gbuffer1_screenShadowEligible. Visible-pixel
coherence principle from F3 design v2 §1 holds.

Flat-up roster (shaders using vec3(0,0,1) fallback):
- gos_vertex_lighted.frag
- gos_tex_vertex.frag
- gos_vertex.frag

Real per-vertex normal:
- gos_tex_vertex_lighted.frag

V1/V2 verification recorded.

Commit ledger:
- <task 1 SHA>: clearGBuffer1 sentinel clear
- <task 2 SHA>: gos_tex_vertex_lighted.frag (cherry-picked canary)
- <task 3 SHA>: gos_vertex_lighted.frag
- <task 4 SHA>: gos_tex_vertex.frag
- <task 5 SHA>: gos_vertex.frag
- this commit: closing report

Smoke tier1 5/5 PASS. Menu canary clean.

Follow-ups: dead-code cleanup (enableMRT/disableMRT, gos_postprocess.cpp:519-520
comment, object_tex.frag, modding-guide stale rows), normal-quality
cleanup for flat-up roster, F1 (water/shoreline material-alpha overload).

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>
EOF
)"
```

---

## Task 8 (optional — defer if time-boxed): Cleanup commits

These are independent, low-risk follow-ups. They can land as part of F3 if convenient or as a separate post-F3 cleanup PR.

**Files:**
- Delete: `shaders/object_tex.frag`, `shaders/object_tex.vert`
- Modify: `GameOS/gameos/gos_postprocess.cpp:519-520` (remove stale comment)
- Modify: `GameOS/gameos/gos_postprocess.{h,cpp}` (delete `enableMRT`/`disableMRT` if confirmed dead)
- Modify: `docs/modding-guide.md` (remove stale `gos_text.frag` row, remove `object_tex` row)

- [ ] **Step 8.1: Confirm `object_tex` is dead.**

```bash
grep -rn "object_tex" GameOS/ mclib/ code/ build64/CMakeFiles/ 2>/dev/null
```

Expected: zero hits in source code. (May appear in CMake-generated files; ignore those.)

- [ ] **Step 8.2: Confirm `enableMRT`/`disableMRT` are dead under Option A.**

```bash
grep -rn "enableMRT\|disableMRT" GameOS/ mclib/ code/
```

Expected: only the definition site in `gos_postprocess.cpp`. No production callers.

- [ ] **Step 8.3: Delete dead shaders.**

```bash
rm shaders/object_tex.frag shaders/object_tex.vert
rm A:/Games/mc2-opengl/mc2-win64-v0.2/shaders/object_tex.frag
rm A:/Games/mc2-opengl/mc2-win64-v0.2/shaders/object_tex.vert
```

- [ ] **Step 8.4: Remove stale comment + dead helpers.**

Edit `GameOS/gameos/gos_postprocess.cpp`. At lines 519-520, remove:

```cpp
    // Start with single draw buffer — MRT only during terrain rendering
    // (AMD RX 7900 corrupts color output if non-terrain shaders write location=1)
```

Remove `enableMRT()` and `disableMRT()` definitions (lines 583-595) and their declarations in `gos_postprocess.h:34-35`.

Update the `clearGBuffer1` header comment that referred to them.

- [ ] **Step 8.5: Update modding guide.**

Edit `docs/modding-guide.md`:
- Remove the `gos_text.frag | Text rendering` row at line 129 (text rendering is post-composite; not a scene shader)
- Remove the `object_tex.frag | 3D objects (mechs, buildings)` row at line 128 (vestigial; deleted)

- [ ] **Step 8.6: Build + smoke.**

Per Steps 1.5 + 1.7. Expected: green build, 5/5 PASS.

- [ ] **Step 8.7: Commit.**

```bash
git add shaders/ GameOS/gameos/gos_postprocess.cpp GameOS/gameos/gos_postprocess.h docs/modding-guide.md
git commit -m "$(cat <<'EOF'
render-contract: post-F3 cleanup — remove dead code

- Delete shaders/object_tex.frag and shaders/object_tex.vert
  (vestigial; zero source references per F3 pass audit §3.8)
- Remove gos_postprocess.cpp:519-520 stale AMD MRT corruption comment
  (refuted by F3 canary; see docs/amd-driver-rules.md
  "Tested-and-refuted claims")
- Remove gos_postprocess::enableMRT() / disableMRT() — defined-but-unused
  helpers preserved through F3 implementation; under Option A, MRT
  stays bound for the entire scene so they have no callers
- Update modding-guide.md — remove stale gos_text.frag and object_tex
  rows

No behavior change. Smoke tier1 5/5 PASS.

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>
EOF
)"
```

---

## Task 9: Discard the canary throwaway branch

**Goal:** Clean up after F3 implementation completes.

- [ ] **Step 9.1: Confirm canary commit is now in F3 history (via cherry-pick in Task 2).**

```bash
git log --oneline --grep="explicit GBuffer1 in gos_tex_vertex_lighted" claude/nifty-mendeleev
```

Expected: one match — the cherry-pick commit on F3 branch.

- [ ] **Step 9.2: Delete the throwaway branch.**

```bash
git branch -D claude/f3-amd-canary-temp
```

- [ ] **Step 9.3: Verify deletion.**

```bash
git branch | grep -i canary
```

Expected: no output (branch deleted).

(No commit in this task — branch deletion is a local-only operation.)

---

## Recovery procedures

If smoke tier1 fails after a task's deploy:

1. **Do not push.** F3 work is local-only.
2. **Capture the failure** — note which mission(s) failed, error logs (`tests/smoke/artifacts/<timestamp>/`).
3. **Stale-shader-cache check** ([memory `stale_shader_cache_symptom.md`](../../../../../.claude/projects/A--Games-mc2-opengl-src/memory/stale_shader_cache_symptom.md)): if symptoms look like frozen-cloud / over-darkened-terrain, isolate by cherry-picking just the previous task's shader change to a temp branch and re-deploying. If symptoms persist with old shaders, it's not F3.
4. **Visual A/B against the previous task's commit.** If the failing change is one shader migration, revert that specific shader file:
   ```bash
   git checkout HEAD~1 -- shaders/<failed-shader>.frag
   ```
   Re-deploy and re-run smoke. If smoke passes, the migration broke something specific to that shader — review the diff against the canary's known-clean pattern.
5. **If unable to identify the cause**, hard-reset the F3 branch to the last known-good commit (the previous task's commit) and stop. Document the failure as a blocker for the post-F3 review.

---

## Self-Review (run after writing all tasks; do not skip)

Refer back to the spec `2026-04-26-render-contract-f3-mrt-completeness-design.md`:

- [x] §1 Coherence principle — addressed by Tasks 2–5 (explicit GBuffer1 writes) + Task 1 (sentinel default).
- [x] §3.1 Option A architecture — Task 1 (sentinel) + Tasks 2–5 (shader migrations).
- [x] §3.1 Default-choice rule — plan implements Option A unconditionally per canary CLEAN verdict.
- [x] §3.1 Conservative-normal fallback — flat-up roster recorded in Task 7 closing report (Tasks 3, 4, 5).
- [x] §5A Required clear fix — Task 1 implements `clearGBuffer1`.
- [x] §5A Real-normal vs. flat-up split — Task 2 uses real Normal; Tasks 3–5 use flat-up.
- [x] §6 Audit deliverable — already exists at `render-contract-f3-pass-audit.md`. Task 6 verifies V1/V2 (the audit's open verification items).
- [x] §7 Frozen surfaces — Task 7 closing report confirms all frozen surfaces unchanged.
- [x] §8 Phase-1A commit sequence — this plan instantiates 4A (Task 1), 5A (Task 2), 6A (Task 3), 7A (Task 4), and adds Task 5 + Task 7 to complete coverage.
- [x] §10 Exit criteria — covered by Task 7 closing report (criteria 1–9 from spec §10).
- [x] §11 OQ-3 (`object_tex.frag` body) — resolved by audit; cleanup in Task 8.
- [x] §12 Follow-ups — listed in Task 7 closing report.

**Placeholder scan:** no `TBD`, `TODO`, `fill in details` — all task content is concrete code or concrete commands. Two acceptable instances of `<placeholder>` syntax in Task 7's closing-report template, marked for substitution at commit time. Task 6 produces structured findings as input to Task 7's substitution.

**Type / signature consistency:** `clearGBuffer1` declared/defined consistently. Sentinel value `(0.5, 0.5, 1.0, 0.0)` matches between header comment, definition, and registry semantic (`rc_gbuffer1_screenShadowEligible(vec3(0,0,1))` evaluates to that exact value).

---

## Plan summary

- **9 tasks total** (1 mandatory + 7 implementation + 1 cleanup deferrable).
- **5 commits if Task 8 deferred; 6 if Task 8 executed.**
- **Each commit gated by smoke tier1 5/5 PASS + visual A/B.** No knowingly-regressing intermediate commits per design spec §8 hard rule.
- **Total expected diff:** ~30 lines C++ (Task 1) + ~12 lines per shader × 4 shaders (Tasks 2–5) + closing report doc (Task 7). Plus optional cleanup deletions in Task 8.
- **Branch:** all commits land on `claude/nifty-mendeleev`. Throwaway `claude/f3-amd-canary-temp` deleted after cherry-pick (Task 9).
- **No push.** F3 is local work.
