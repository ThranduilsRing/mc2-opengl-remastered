# Render Contract F3 — Visible-Pixel GBuffer Coherence (Design v2)

**Status:** Spec, awaiting review (v2 rewrite — central principle reframed)
**Date:** 2026-04-26
**Branch:** `claude/nifty-mendeleev`
**Predecessor:** [Render Contract Registry phase 1](2026-04-26-render-contract-registry-design.md) — landed at `5256659`
**Closing report (registry):** [`render-contract-registry-report.md`](render-contract-registry-report.md)

---

## 0. Why this is a v2

The first draft of this spec framed F3 as "state cleanup" — disable MRT after the load-bearing clear, scope MRT around known `GBuffer1` writers (terrain, overlays, decals, grass, static props), leave the five incomplete shaders untouched. Self-review and operator review surfaced two compounding flaws in that framing:

1. `glClearColor` sets `alpha=1.0` in both gameplay and menu paths. Without an explicit `glClearBufferfv(GL_COLOR, 1, ...)` to a registry sentinel, attachment 1 ends up at alpha=1.0 everywhere → `rc_pixelHandlesOwnShadow` returns true → post-shadow skipped on every non-terrain pixel.
2. Even with an explicit sentinel clear, **terrain overdrawn by world-space objects** is the killer case. The terrain pass writes `GBuffer1.alpha=1.0` legitimately. A mech later passes the depth test and writes `COLOR0` + depth at the same screen pixel — but in COLOR0-only mode it does not touch `GBuffer1`, so the mech pixel inherits the **stale terrain mask = 1.0**. `shadow_screen.frag` reads "shadow handled" and skips post-process shadow on the mech. **Visible regression on every mech, vehicle, and building pixel that overlaps terrain in screen space.**

Today's behavior dodges both problems by accident: MRT stays bound for the entire scene, and AMD's RX 7900 driver writes `vec4(0,0,0,0)` to undeclared MRT outputs — the "lucky default" that gives mech pixels `GBuffer1.alpha=0.0` so post-shadow applies. The fix that the original `enableMRT`/`disableMRT` design *intended* breaks the lucky default without replacing it.

F3 is therefore not a one-line state fix. The central problem is **visible-pixel GBuffer coherence**: for every pixel `shadow_screen.frag` samples, `GBuffer1` must describe the same surface that wrote `COLOR0` and depth. Any solution must guarantee coherence under all overdraw scenarios.

---

## 1. The coherence principle

> **F3 coherence principle.** For every visible pixel that `shadow_screen.frag` consumes, `GBuffer1` must correspond to the surface that wrote `COLOR0` and `depth`. A pass that updates `COLOR0`/depth must either update `GBuffer1` accordingly (explicitly, with a registry helper) or be guaranteed not to be visible in the post-shadow pass.

This is stronger than v1's "scope MRT around writers." It mandates that *coherence be designed in*, not inherited from driver behavior.

A solution satisfying the principle must answer four questions:

- **Q1.** Which passes can depth-overdraw a previous `GBuffer1` writer (i.e., write `COLOR0`/depth on top of a pixel a prior pass wrote with MRT bound)?
- **Q2.** Which of those overdrawing passes produce pixels that `shadow_screen.frag` consumes (i.e., depth < 1.0 and visible after composition)?
- **Q3.** For each such pass, what `GBuffer1` value correctly describes its surface? (`rc_gbuffer1_screenShadowEligible` for objects that should receive post-shadow; `rc_gbuffer1_shadowHandled` for surfaces that handle their own shadowing.)
- **Q4.** Is the AMD `location=1` corruption claim real enough to forbid explicit writes from those passes?

Q1–Q3 are answerable by codebase audit. Q4 is answerable only by empirical canary. The canary's outcome **forks the F3 architecture**: it is no longer optional out-of-band research; it is a phase-0 prerequisite.

---

## 2. Audit findings (factual; preserved from v1)

### 2.1 `enableMRT` / `disableMRT` are defined-but-uncalled

Confirmed via grep (`enableMRT|disableMRT`) across the worktree:

- Definitions at [`gos_postprocess.cpp:583`](../../../GameOS/gameos/gos_postprocess.cpp) (enable) and `:591` (disable).
- Header declarations at [`gos_postprocess.h:34-35`](../../../GameOS/gameos/gos_postprocess.h) with comments "call before terrain draws" / "call after terrain draws".
- **Zero callsites** in `gameos_graphics.cpp`, `gameosmain.cpp`, or any production source. Only references outside the function bodies are in planning docs.

### 2.2 `beginScene` state transition (the actual bug shape)

Frame trace at HEAD `5256659`:

| Step | File:line | State after |
|---|---|---|
| `pp->beginScene()` enters | [`gameosmain.cpp:405`](../../../GameOS/gameos/gameosmain.cpp) | — |
| `glBindFramebuffer(GL_FRAMEBUFFER, sceneFBO_)` | [`gos_postprocess.cpp:518`](../../../GameOS/gameos/gos_postprocess.cpp) | scene FBO bound |
| `glDrawBuffers(2, {COLOR0, COLOR1})` | `gos_postprocess.cpp:521-525` | **MRT bound** |
| `pp->beginScene()` returns | — | **MRT still bound** |
| `glClear(COLOR\|DEPTH)` (alpha=1.0) | `gameosmain.cpp:409` | clears both attachments to glClearColor (alpha=1.0) |
| sky-color `glClear(COLOR\|DEPTH\|STENCIL)` (alpha=1.0) | `gameosmain.cpp:456` | re-clears both to (sky color, alpha=1.0) |
| Terrain tess draw | `gameos_graphics.cpp:2987-2998` | MRT bound; writes `GBuffer1.alpha=1.0` for terrain pixels |
| `pp->markTerrainDrawn()` | `gameos_graphics.cpp:2997` | `sceneHasTerrain_=true` |
| Terrain overlays / decals / grass / static props | various | MRT bound; correct `GBuffer1` writes |
| **TGL world objects (mechs, buildings, vehicles)** | TGL render paths | **MRT bound, but shaders are incomplete (`layout(location=0)` only)** — relies on AMD writing `vec4(0,0,0,0)` to attachment 1 for post-shadow eligibility |
| **gosFX particles** | various | **same: incomplete shaders, relies on AMD lucky default** |
| HUD / text submission | (audit pending) | likely drawn after compositing to default FB |
| `pp->runScreenShadow()` | `gos_postprocess.cpp:598+` | rebinds different FBO; samples both attachments |

The intent comment at [`gos_postprocess.cpp:519-520`](../../../GameOS/gameos/gos_postprocess.cpp) — *"Start with single draw buffer — MRT only during terrain rendering (AMD RX 7900 corrupts color output if non-terrain shaders write location=1)"* — describes a design that was *supposed* to be implemented. It was not.

### 2.3 The five incomplete shaders

Each declares only `layout(location=0) out vec4 FragColor`:

| Shader | Used for | Likely overlaps terrain in screen space? |
|---|---|---|
| [`gos_vertex.frag`](../../../shaders/gos_vertex.frag) | Untextured colored vertex draws | yes (basic geometry; line/path overlays) |
| [`gos_vertex_lighted.frag`](../../../shaders/gos_vertex_lighted.frag) | Lit untextured vertex draws | yes |
| [`gos_tex_vertex.frag`](../../../shaders/gos_tex_vertex.frag) | Textured + animated water + IS_OVERLAY bridge | yes |
| [`gos_tex_vertex_lighted.frag`](../../../shaders/gos_tex_vertex_lighted.frag) | Lit textured (TGL world objects: mechs, buildings, vehicles) | **yes — primary overdraw concern** |
| [`object_tex.frag`](../../../shaders/object_tex.frag) | Modding-guide labels "3D objects (mechs, buildings)"; current body `c + vec3(1,1,1)` is suspicious — see §11 OQ-3 | yes |

### 2.4 `gos_text.frag` does not exist

Glob `shaders/**/*text*.frag` returns no files. `docs/modding-guide.md:129` reference is stale.

### 2.5 AMD corruption claim — soft constraint, gating-decision input

The claim at `gos_postprocess.cpp:519-520` is **not** in [`docs/amd-driver-rules.md`](../../amd-driver-rules.md), which lists 8 confirmed-and-tested rules. The operator's recall is uncertain. The remembered AMD behavior may relate to the documented `Attribute 0 must be active` rule (vertex attribute, not fragment output).

**F3 v2 escalates the canary from out-of-band research to a phase-0 prerequisite** because the canary's outcome decides which architecture (Option A or Option Hybrid) is viable.

### 2.6 `markTerrainDrawn()` is not the MRT boundary

`pp->markTerrainDrawn()` at `gameos_graphics.cpp:2997` is a terrain-presence signal for the next frame's clear-color choice. F3 must not couple MRT toggling to it. The authoritative contract is the per-pass `requiresMRT` property.

---

## 3. The three viable architectures

The coherence principle (§1) plus the audit (§2) constrain the design space to three options. Each is evaluated against Q1–Q4.

### 3.1 Option A — Explicit `GBuffer1` writes from all overdrawing shaders

**Mechanism.** Add `layout(location=1) out vec4 GBuffer1;` to every shader confirmed to overdraw terrain in scene-FBO space. Each shader writes via the registry's `rc_gbuffer1_screenShadowEligible(normal)` (or the flat-up variant if no normal is available). Keep MRT bound for the entire scene FBO phase, exactly like today.

**Coherence guarantee.** Strong. Every pass that writes `COLOR0`/depth also writes `GBuffer1` with a value matching its semantic intent. No pixel can inherit a stale mask from a prior pass.

**Source touch.** Five fragment shaders modified to declare and write `GBuffer1`. Possibly more if particles or UI/HUD shaders are confirmed to overdraw terrain. No FBO state-machine change.

**AMD risk.** **Gated by canary.** The comment at `gos_postprocess.cpp:519-520` warns that this exact pattern corrupts on AMD RX 7900. If the warning is real, Option A is forbidden. If it is stale, Option A is the cleanest design and aligns with the registry's "every writer routes through `rc_*` helpers" mental model.

**Verdict.** Preferred if canary clean. Forbidden if canary confirms corruption.

**Default-choice rule:** if the canary is clean, choose Option A **unless** the §6 pass audit surfaces a shader that cannot provide even a conservative `GBuffer1` value. The temptation to pick Hybrid because "it touches fewer shaders" is not a valid reason; A is the architecturally correct path and Hybrid's coherence guarantee is conditional.

**Conservative-normal fallback.** Where a Group II shader has no real surface normal (textured-only paths, simple vertex-color paths), the migration writes `rc_gbuffer1_screenShadowEligible(vec3(0.0, 0.0, 1.0))` (flat-up). **Flat-up is a compatibility fallback, not a physically correct object normal.** Any shader migrated with flat-up must be listed in the F3 closing report so a later normal-quality cleanup can revisit it once a downstream pass starts consuming `GBuffer1.rgb` as world normal. Do not pretend the RGB contract is fully solved by Option A's first roll-out.

### 3.2 Option Hybrid — Scoped MRT + incomplete shaders preserved

**Mechanism.** Disable MRT post-clear (`{COLOR0}` becomes default). Scope MRT (`enableMRT`/`disableMRT`) around two distinct kinds of pass:

- **Group I — registry-explicit writers** (terrain, overlays, decals, grass, static prop): shaders declare `GBuffer1` and write registry helpers.
- **Group II — overdrawing incomplete shaders** (TGL world objects, particles, possibly more): shaders are *not* modified. They still don't declare `layout(location=1)`. But they run with MRT bound, so AMD's "lucky default" `vec4(0,0,0,0)` lands in attachment 1 for their pixels.

Add explicit `glClearBufferfv(GL_COLOR, 1, sentinel)` post-main-clear so any pixel never overwritten by Group I or II inherits a defined sentinel rather than `glClearColor`'s alpha=1.0.

**Coherence guarantee.** Conditional. Coherence holds **only if AMD's undeclared-MRT-output behavior is `vec4(0,0,0,0)`** for every Group II shader. This is what currently ships and apparently works on the developer's hardware. F3 v2 would make this dependence **explicit, scoped, and auditable** rather than ambient — but it does not eliminate the dependence.

**Source touch.** No shader changes. Several pass-level `enableMRT`/`disableMRT` brackets in renderer code. New `clearGBuffer1` helper. New `disableMRT` post-clear call.

**AMD risk.** **None new.** Hybrid relies on the same AMD behavior that ships today; it just scopes the reliance to Group II passes instead of "everything in the scene."

**Verdict.** Preferred if canary corrupts. Acceptable as a transitional state if canary clean (then a follow-up F-something migrates Group II to Option A, which becomes "drop the Group II MRT scope and write `GBuffer1` explicitly").

**Hybrid is acceptable only as a compatibility containment step, not as the desired end state.** It formalizes today's driver-dependent behavior and narrows where that dependency lives, but it does not eliminate the dependency. Any spec that ships F3 as Hybrid must enumerate the migration path to Option A as a follow-up.

### 3.3 Option C-prime (v1's design) — REJECTED

**Mechanism.** Disable MRT post-clear. Scope MRT around registry-explicit writers only. Leave incomplete shaders as `layout(location=0)` and let them run in `{COLOR0}`-only mode.

**Coherence failure.** Mech draws over terrain in COLOR0-only mode → mech pixel inherits terrain's `GBuffer1.alpha=1.0` → post-shadow skipped on mech → **visible regression**. The explicit sentinel clear does not help because terrain overwrote it before the mech draws.

**Verdict.** **Rejected.** Listed here for completeness; it was the v1 design and did not satisfy the coherence principle.

### 3.4 Option Stencil-mechanism — DEFERRED, out of scope

**Mechanism.** Use the stencil buffer (already cleared at `gameosmain.cpp:456`) to differentiate "terrain pixel" from "object pixel." Post-shadow gates on stencil, not on `GBuffer1.alpha`. This decouples coherence from the FBO state machine.

**Why deferred.** Requires modifying [`shadow_screen.frag`](../../../shaders/shadow_screen.frag), which §4 freezes. Also requires every terrain/object pass to set up stencil writes — a wider refactor than F3 should attempt. Listed here as a future direction (§12 follow-ups).

---

## 4. The phase-0 canary (gating prerequisite)

The canary determines whether F3 implements Option A or Option Hybrid. **No code beyond the canary itself is committed until the canary completes.**

### 4.1 Protocol

1. **Throwaway branch.** Create `claude/f3-amd-canary-temp` from current HEAD. **Do not push. Do not merge.** The canary's only output is a doc commit recording findings.

2. **Single-shader test.** Add `layout(location=1) out vec4 GBuffer1;` to one shader that draws over terrain extensively. **Recommended: [`gos_tex_vertex_lighted.frag`](../../../shaders/gos_tex_vertex_lighted.frag)** — primary TGL path, large pixel coverage, easiest to spot corruption.

   Have it write the registry helper:
   ```glsl
   #include "include/render_contract.hglsl"
   ...
   GBuffer1 = rc_gbuffer1_screenShadowEligible(vec3(0.0, 0.0, 1.0));  // flat up + post-shadow eligible
   ```

3. **Build + deploy.** Standard `/mc2-build-deploy` cycle to `A:/Games/mc2-opengl/mc2-win64-v0.2/`.

4. **Observation pass.** Run two missions:
   - One mission with heavy mech presence (e.g., `mc2_10` or `mc2_17` from the tier1 set).
   - One mission with significant infrastructure (mc2_03 or mc2_24).

   Watch for:
   - **Color-channel corruption** on mech pixels (the comment's stated failure mode).
   - **Framebuffer artifacts** — striping, swizzles, garbage color.
   - **Full-frame draw drops** (the documented "AMD skips draws" failure mode of the attribute-0 rule, in case it applies to fragment outputs).
   - **Post-shadow correctness** — mechs should darken in shadow exactly as today (alpha=0 from explicit write matches the previous "lucky default").

5. **Outcomes:**

   | Observation | F3 architecture |
   |---|---|
   | No corruption, post-shadow correct | **Option A.** AMD claim is stale or mis-remembered. Update `docs/amd-driver-rules.md` to record "Tested 2026-04-26, not observed." Canary commit becomes the first commit of F3 implementation (then expand to other shaders). |
   | Corruption observed | **Option Hybrid.** AMD claim is real. Promote to a numbered rule in `docs/amd-driver-rules.md` with the test setup as evidence. Revert canary; proceed with Option Hybrid implementation. |
   | Inconclusive (subtle visual changes that could be either) | **Inspect with RGP / RenderDoc.** If still inconclusive, default to Hybrid (safer); document the inconclusiveness. |

6. **Doc commit (canary outcome).** Write `docs/superpowers/specs/render-contract-f3-canary-report.md` recording the test setup, observations, and outcome. This is committed to the F3 branch as canary commit 1.

7. **Branch hygiene.** If outcome is "no corruption," the canary's shader change can be cherry-picked as the start of Option A implementation. If outcome is "corruption," `git reset --hard` the canary branch and discard.

### 4.2 What the canary cannot tell us

- Whether *every* incomplete shader is safe under Option A. The canary tests one shader (the highest-pixel-coverage one). Option A implementation must roll out shader-by-shader with the same observation discipline.
- Whether other AMD GPUs / driver versions behave the same. The developer's RX 7900 with driver 26.3.1 is the reference. Any future hardware change re-opens the question.
- Whether coherence is correctly preserved by the explicit write semantics — that requires §6 audit work regardless of canary outcome.

---

## 5. Architecture under each canary outcome

The two outcomes are described as forked specifications. The implementation plan written after the canary picks one fork and discards the other.

### 5A. If canary clean: Option A architecture

**Frame state.** Same as today: MRT bound from `beginScene` through `runScreenShadow`. No `disableMRT` calls in production code. The `enableMRT`/`disableMRT` helpers may be retained for future use or deleted as dead code (decision deferred).

**Required clear fix.** Even under Option A, the explicit `glClearBufferfv(GL_COLOR, 1, {0.5, 0.5, 1.0, 0.0})` after the main clears is **mandatory**. Reason: a pixel covered only by the sky-color `glClear` (no draw lands on it) gets `glClearColor` alpha=1.0 → "shadow handled." Sky pixels are usually depth=1.0 (skipped by `shadow_screen`), so this is benign in practice — but the registry's coherence guarantee should not depend on the depth-skip side path. Set the sentinel explicitly.

**Shader changes.** Every shader confirmed to overdraw terrain on the scene FBO declares `layout(location=1) out vec4 GBuffer1` and writes via a registry helper:

| Shader | Helper | Notes |
|---|---|---|
| `gos_tex_vertex_lighted.frag` | `rc_gbuffer1_screenShadowEligible(N)` if normal is available; `rc_gbuffer1_screenShadowEligible(vec3(0,0,1))` (flat-up) otherwise | TGL world-object path |
| `object_tex.frag` | `rc_gbuffer1_screenShadowEligible(vec3(0,0,1))` | suspicious body — see OQ-3 |
| `gos_tex_vertex.frag` | `rc_gbuffer1_screenShadowEligible(vec3(0,0,1))` | IS_OVERLAY bridge; water/shoreline overlay; F1 will revisit semantics |
| `gos_vertex.frag` | `rc_gbuffer1_screenShadowEligible(vec3(0,0,1))` | basic colored vertex |
| `gos_vertex_lighted.frag` | `rc_gbuffer1_screenShadowEligible(vec3(0,0,1))` | lit untextured |
| gosFX particle shaders (audit per-shader) | depends on usage | particles often blend over terrain; `screenShadowEligible` matches today's behavior |

**Pass classification (Option A):** every scene-FBO draw is implicitly `requiresMRT=true`. The set of `requiresMRT=false` passes is empty (or limited to passes drawn outside the scene FBO).

**Coherence proof.** Trivial: every pass writes `GBuffer1` per its semantic. Last writer wins (depth-sorted), which is exactly what `shadow_screen` should sample.

### 5B. If canary corrupts: Option Hybrid architecture

**Frame state.** New post-clear transition installs default `{COLOR0}` mode. MRT is scoped per pass.

| Frame stage | Owner | Draw buffers |
|---|---|---|
| `pp->beginScene()` bind | gameosmain.cpp:405 → gos_postprocess.cpp:510 | `{COLOR0, GBUFFER1}` |
| Two `glClear` calls in gameosmain.cpp | lines 409, 456 | clears both attachments |
| **`pp->clearGBuffer1()` (NEW)** | gameosmain.cpp post-line-456 | `{COLOR0, GBUFFER1}` (MRT still bound); writes sentinel `(0.5, 0.5, 1.0, 0.0)` to attachment 1 |
| **`pp->disableMRT()` (NEW)** | gameosmain.cpp post-clearGBuffer1 | `{COLOR0}` becomes default scene-draw state |
| Group I passes (TerrainBase, TerrainOverlay, TerrainDecal, Grass, StaticProp) | bracket each draw with `enableMRT`/`disableMRT` | `{COLOR0, GBUFFER1}` during each pass |
| **Group II passes (TGL world objects, particles, possibly debug overlays)** | bracket each draw with `enableMRT`/`disableMRT` | `{COLOR0, GBUFFER1}` during each pass; shader is incomplete; relies on AMD writing zeros to undeclared output |
| `runScreenShadow` and post passes | own FBOs | unchanged |

**Pass classification (Option Hybrid):**

- **Group I — `requiresMRT=true`, registry-explicit:** TerrainBase, TerrainOverlay, TerrainDecal, Grass, StaticProp.
- **Group II — `requiresMRT=true`, incomplete (relies-on-AMD-zero-default):** TGL object passes (`gos_tex_vertex_lighted.frag`, `object_tex.frag`, `gos_tex_vertex.frag`), gosFX particle passes, any other scene-FBO pass identified by the §6 audit that overdraws terrain.
- **`requiresMRT=false`:** any pass whose audit confirms it does not overdraw a Group I or Group II pixel that `shadow_screen` consumes. Default-empty until proven.

**Coherence proof (Hybrid, conditional).** Group I passes write `GBuffer1` correctly via registry helpers. Group II passes write `GBuffer1` to `vec4(0,0,0,0)` via AMD's undeclared-output behavior — **assumed**, not provably correct by spec. Pixels covered by neither group inherit the explicit sentinel `(0.5, 0.5, 1.0, 0.0)`. Coherence holds **iff the AMD assumption holds**. If the assumption fails on a different driver/GPU, Hybrid breaks.

**Hybrid is the strict superset of today's behavior** (which is "everything is `requiresMRT=true`" implicitly) refined only in that the FBO state is now well-ordered and auditable. It does not improve coherence; it formalizes existing reliance.

### 5C. Comparison table

| | Option A | Option Hybrid |
|---|---|---|
| Coherence guarantee | strong (explicit writes) | conditional (depends on AMD undeclared-output behavior) |
| AMD risk | gated by canary | same as today |
| Source touch | 5–6 shader files modified | renderer state machine restructured; no shader changes |
| Future-proof against driver changes | yes | no |
| Aligns with registry's "every writer routes through `rc_*`" model | yes | no (Group II writers are implicit) |
| Phase-1-inert in pixel output? | yes (sentinel matches AMD lucky default) | yes (preserves current MRT-bound state where it matters) |

---

## 6. The §3.4 / Q1-Q3 audit (must complete before implementation)

Independent of canary outcome, the audit answers Q1–Q3 from §1. Both Option A and Option Hybrid use the audit's pass list — A to know which shaders to modify, Hybrid to know which passes to bracket.

### 6.1 Audit task

For every fragment shader bound to draws on the scene FBO between `pp->beginScene()` and `pp->runScreenShadow()`:

1. **Identify the shader and its callsite(s)** in the renderer source.
2. **Determine whether its draws can pass depth on top of pixels written by Group I passes** (terrain/overlay/decal/grass/static-prop). Most world-space draws qualify. Pure 2D HUD/UI on default FB after compositing does not.
3. **Determine whether the resulting pixels are sampled by `shadow_screen.frag`** (depth < 1.0 after composition).
4. **Classify into:**
   - `Group I` if shader already writes `GBuffer1` via registry helper.
   - `Group II-Opaque` if shader is incomplete, writes depth, and overdraws terrain. (Option A target for shader modification; Option Hybrid target for MRT scope.)
   - `Group II-Blend` if shader is incomplete, blended (typical particle/effect — `glBlendFunc` non-default and/or `glDepthMask(GL_FALSE)`), and may or may not "own" the visible pixel. Audit `glDepthMask` state, `glBlendFunc` state, and whether `shadow_screen` would sample the resulting pixel as belonging to this surface or to the surface beneath. **Do not blanket-migrate Group II-Blend without confirming per-shader behavior.**
   - `Excluded` if pass does not draw to scene FBO, or does not overdraw terrain, or is not sampled by `shadow_screen`. Document why.

### 6.2 Audit deliverable

`docs/superpowers/specs/render-contract-f3-pass-audit.md` recording:

- Per-shader callsite inventory (file:line, render-state context, draw entry point).
- Group classification (I / II / Excluded) with rationale.
- Anticipated `GBuffer1` write semantic per Group II shader (for Option A: which `rc_*` helper; for Option Hybrid: relies-on-AMD-zero, no helper).
- Open questions per shader for canary observation focus.

This document is committed as F3 implementation commit 1 (regardless of which option wins).

### 6.3 Anticipated results

Strong prior: TGL world-object paths (`gos_tex_vertex_lighted.frag`, `object_tex.frag`, possibly `gos_tex_vertex.frag` for water overlays) are **Group II-Opaque** — they write depth and own their pixels. gosFX particle shaders are **Group II-Blend** — they typically run with `glDepthMask(GL_FALSE)` and additive/translucent blend, so they do not own depth and may be safe to leave at `requiresMRT=false` (the underlying surface remains the depth-owning pixel). HUD/UI/text are likely Excluded (drawn after compositing). Debug overlays are pass-by-pass.

Audit may surface surprises (e.g., a shader that overdraws terrain but has been incorrectly assumed to be 2D-only, or a particle path that does write depth). The point of running the audit is to catch them.

---

## 7. Frozen surfaces (do not touch in F3)

- **The 8 projectZ wrappers in [`mclib/camera.h`](../../../mclib/camera.h).** Load-bearing per registry spec.
- **The Render Contract Registry helpers** ([`shaders/include/render_contract.hglsl`](../../../shaders/include/render_contract.hglsl), [`mclib/render_contract.{h,cpp}`](../../../mclib/render_contract.h)). F3 uses existing helpers; does not extend the API.
- **Shaders already writing `GBuffer1` via registry helpers** (`gos_terrain.frag`, `decal.frag`, `gos_grass.frag`, `terrain_overlay.frag`, `static_prop.frag`). Untouched.
- **`shadow_screen.frag` reader-side logic.** F3 fixes the producer side; consumer threshold and gate logic is unchanged. (Stencil-mechanism alternative §3.4 is therefore deferred.)
- **The grep census** [`scripts/check-render-contract-gbuffer1.sh`](../../../scripts/check-render-contract-gbuffer1.sh). Continues to enforce helper-only `GBuffer1` writes. Option A adds new writes; all go through `rc_*` helpers, so census continues to pass.
- **`docs/amd-driver-rules.md` rules 1–8.** Untouched. The canary's outcome may add a rule 9 (corruption confirmed) or a "Tested" footnote on the existing comment (corruption refuted).

---

## 8. Commit sequence (gated on canary outcome)

The first commits are option-agnostic. The fork happens at canary commit.

### Phase 0 — option-agnostic prep

**Commit 1 — This spec lands.** Doc-only.

**Commit 2 — Pass audit.** `docs/superpowers/specs/render-contract-f3-pass-audit.md` per §6. Doc-only. May note "Anticipated Group II per audit; shader-by-shader confirmation in canary observation."

**Commit 3 — Canary report.** Run §4 protocol on a throwaway branch; revert; commit `docs/superpowers/specs/render-contract-f3-canary-report.md` summarizing observations and verdict (clean / corrupt / inconclusive). Doc-only on the F3 branch.

After commit 3 the architecture is decided. Implementation forks below.

### Phase 1A — Option A implementation (canary clean)

**Commit 4A — Sentinel clear (mandatory under both options).** Add `clearGBuffer1()` helper writing `(0.5, 0.5, 1.0, 0.0)`; call from `gameosmain.cpp` post-line-456. No `disableMRT` call (Option A keeps MRT bound). Smoke tier1 5/5 PASS; visual A/B clean.

**Commit 5A — Migrate first incomplete shader to explicit `GBuffer1` write.** Pick the canary's target shader (`gos_tex_vertex_lighted.frag`); add `layout(location=1)` and `rc_gbuffer1_screenShadowEligible(...)` call. Smoke + visual A/B per shader.

**Commits 6A, 7A, ... — One shader per commit.** Each Group II shader migrated independently. Per-commit visual A/B confirms no regression. Order: highest-pixel-coverage first (TGL world objects, then particles, then small overlays).

**Final 1A commit — Census + closing report.** Update `scripts/check-render-contract-gbuffer1.sh` if it needs to recognize the new shader writes. Write `docs/superpowers/specs/render-contract-f3-report.md` summarizing.

### Phase 1H — Option Hybrid implementation (canary corrupts or inconclusive)

**Commit 4H — `clearGBuffer1` helper.** Same as 4A but no behavior change yet (helper unused).

**Commit 5H — Atomic state-transition fix + Group I scope.** In one commit (avoids knowingly-broken intermediate):
- `gameosmain.cpp` post-line-456: `pp->clearGBuffer1(); pp->disableMRT();`
- Group I pass scopes (TerrainBase, TerrainOverlay, TerrainDecal, Grass, StaticProp) bracketed with `enableMRT`/`disableMRT` and `[RENDER_CONTRACT:requiresMRT Pass=...]` tags.

Smoke + visual A/B. **Will regress on Group II pixels** — known intentional, fixed by next commit.

**Commit 6H — Group II scope.** Bracket each Group II pass identified by §6 audit (TGL world-object paths, particle paths, etc.) with `enableMRT`/`disableMRT` + tags. Smoke + visual A/B should now match HEAD.

**Commit 7H — Instrumentation + grep census.** `MC2_RENDER_CONTRACT_TRACE` env-gated logger (§9); new `scripts/check-render-contract-mrt-pairs.sh` static check.

**Final 1H commit — Closing report.**

**Note on commits 5H / 6H ordering.** Splitting them produces a knowingly-regressing intermediate (Group II pixels lose post-shadow until commit 6H). The operator's preference is "green after every commit."

**Hard rule:** **No knowingly-regressing commit may be deployed or treated as smoke-gated.** The implementation plan must fuse any commits that are individually known to create visible regression. The conceptual split between 5H and 6H may be preserved in the spec for review readability, but the actual git commits must be atomic where regression-pairing exists. The CI / smoke-tier1 gate runs against deploy targets; any commit that would fail that gate by design fails the rule and must be combined.

---

## 9. Instrumentation — `MC2_RENDER_CONTRACT_TRACE`

(Spec retained from v1 with one addition: trace events under Option A vs Option Hybrid differ.)

**Env var:** `MC2_RENDER_CONTRACT_TRACE=1`. Default off. Banner contributes to `[INSTR v1] enabled: ...`.

**Schema (versioned `v1`) under Option Hybrid:**

```
[RC_MRT v1] event=clearReady drawBuffers=2
[RC_MRT v1] event=postClear drawBuffers=1 default=COLOR0
[RC_MRT v1] event=enable pass=TerrainBase site=gosRenderer_terrainDrawIndexedPatches drawBuffers=2
[RC_MRT v1] event=disable pass=TerrainBase site=gosRenderer_terrainDrawIndexedPatches drawBuffers=1
[RC_MRT v1] event=enable pass=TGLWorldObject site=TG_Shape_Render drawBuffers=2
[RC_MRT v1] event=disable pass=TGLWorldObject site=TG_Shape_Render drawBuffers=1
```

**Schema under Option A:** since MRT stays bound for the whole scene, only the clear transitions emit events:

```
[RC_MRT v1] event=clearReady drawBuffers=2
[RC_MRT v1] event=sentinelClear attachment=1 value=(0.5,0.5,1.0,0.0)
[RC_MRT v1] event=defaultDuringScene drawBuffers=2
```

**Mismatch summary** (Hybrid only — irrelevant under A):

```
[RC_MRT v1] mismatch frame=123 enableCount=5 disableCount=4 currentDrawBuffers=2 lastPass=TGLWorldObject lastSite=TG_Shape_Render
```

Static check: `scripts/check-render-contract-mrt-pairs.sh` (Hybrid only). Limitations as in v1: best-effort grep, runtime trace is the authoritative balance check.

---

## 10. Exit criteria

**Common (both options):**

1. **Coherence guarantee documented.** Closing report explicitly proves that for every visible pixel `shadow_screen.frag` consumes, `GBuffer1` describes the same surface as `COLOR0`/depth.
2. **Smoke tier1 5/5 PASS** after each commit.
3. **Visual A/B regression gate** vs HEAD `5256659` shows no shadow / shading / coloration regression on at least: one mech-heavy mission, one particle-heavy mission, one mission with significant static-prop and decal content.
4. **Trace cleanliness.** Run with `MC2_RENDER_CONTRACT_TRACE=1` (Hybrid) over a tier1 sweep produces no `mismatch` lines. (Under A, no mismatch is possible.)
5. **Census passes.** `scripts/check-render-contract-gbuffer1.sh` exits 0. Under Hybrid, `scripts/check-render-contract-mrt-pairs.sh` also exits 0.
6. **Frozen surfaces unchanged.**
7. **Doc parity.** This spec, the audit doc (§6), the canary report (§4), the closing report, and `docs/render-contract.md` are mutually consistent.

**Option A specific:**

8. Every Group II shader from the audit declares `layout(location=1)` and writes via a registry helper.
9. `docs/amd-driver-rules.md` updated to record canary outcome (claim refuted / footnoted).

**Option Hybrid specific:**

8. Every Group II pass identified by the audit is bracketed with `enableMRT`/`disableMRT` and tagged `[RENDER_CONTRACT:requiresMRT Pass=<name>]`.
9. `docs/amd-driver-rules.md` updated with rule 9 documenting the confirmed AMD MRT corruption (or footnote on the existing comment if inconclusive).
10. Closing report acknowledges the conditional coherence guarantee and lists Hybrid → A migration as a future follow-up.

---

## 11. Open questions

**OQ-1 — RESOLVED in §1 / §3.** Attachment-1 alpha value after sky-color clear is `1.0`; sentinel clear is mandatory under both options. Coherence requirements drive design; no longer an open question.

**OQ-2 — Pass classification audit (§6).** Q1–Q3 of §1 are answered by the audit deliverable. Resolution: produce `render-contract-f3-pass-audit.md` as commit 2.

**OQ-3 — `object_tex.frag` body anomaly.** `FragColor = vec4(c + vec3(1,1,1), 1.0)` saturates to white. If mechs render correctly today, the shader is either unused in the production path or the formula is intentional in a way the spec does not understand. **Out of scope for F3 mechanism choice** but in scope for F3 audit (§6) — audit must determine whether this shader is actually bound for production draws and, if so, what `GBuffer1` value it should write under Option A.

**OQ-4 — `gos_text.frag` reference.** Modding-guide doc bug. **Out of scope.**

**OQ-5 — RAII helper.** `RenderMRTScope` deferred under Option Hybrid; not relevant under Option A.

**OQ-6 — Audit completeness.** §6 audit is best-effort. The ambient discipline of tracing every scene-FBO draw to a known shader is high-effort. **Mitigation:** under Option Hybrid, runtime trace catches missed Group II passes (mismatch lines). Under Option A, missed passes manifest as visible regressions, which the visual A/B gate catches.

**OQ-7 — Canary scope.** §4 tests one shader (`gos_tex_vertex_lighted.frag`). Other shaders may behave differently on AMD. **Mitigation:** under Option A, each Group II shader migration commit (§8 1A) is its own visual A/B gate, so corruption in any individual shader surfaces independently.

---

## 12. Follow-ups

- **F1.** Water/shoreline material-alpha overload of the post-shadow mask (registry §3.1).
- **F2.** Legacy "terrain flag" terminology cleanup.
- **F4.** Centralized shadow-eligibility predicate for objects.
- **F5.** Debug-overlay enforcement (RAlt+P projectZ overlay; registry §3.5).
- **Stencil-mechanism alternative.** Decouple coherence from `GBuffer1.alpha` by using stencil. Out of scope for F3 (requires `shadow_screen.frag` change), but the right long-term direction if Hybrid's conditional guarantee proves brittle.
- **Hybrid → A migration.** If F3 ships Hybrid, follow-up migrates Group II shaders to explicit `GBuffer1` writes once the canary's outcome is confirmed-stable on a wider hardware set.
- **Post-F3 doc consolidation.** Update `docs/modding-guide.md` to remove stale `gos_text.frag` row and clarify `object_tex.frag` usage (OQ-3, OQ-4).
- **`object_tex.frag` audit.** Investigate whether `c + vec3(1,1,1)` is vestigial, a bug, or intentional (OQ-3).

---

## 13. Go / no-go criterion

**This spec is implementation-ready when:**

> The revised spec explicitly guarantees that visible `COLOR0` and `depth` and `GBuffer1` describe the same surface for every pixel `shadow_screen.frag` reads.

This guarantee is provided in §3 by the option-tree analysis and §5 by the per-option coherence proof:

- **Option A** provides the guarantee unconditionally (every overdrawing pass writes `GBuffer1` explicitly).
- **Option Hybrid** provides the guarantee conditionally on AMD's undeclared-MRT-output behavior. The §4 canary's outcome controls whether the conditional guarantee is acceptable.

The guarantee is **not** provided by the architecture choice alone; it requires the §6 audit to enumerate Group II passes correctly. Implementation cannot proceed past commit 3 (canary report) until commit 2 (audit) and commit 3 (canary) are both complete and the option fork is decided.

---

## 14. References

- Registry spec: [`2026-04-26-render-contract-registry-design.md`](2026-04-26-render-contract-registry-design.md)
- Registry closing report: [`render-contract-registry-report.md`](render-contract-registry-report.md)
- Callsite inventory (with §3.2 confirmation): [`render-contract-callsite-inventory.md`](render-contract-callsite-inventory.md)
- Existing render-contract doc (submission-space; orthogonal): [`../../render-contract.md`](../../render-contract.md)
- AMD driver rules: [`../../amd-driver-rules.md`](../../amd-driver-rules.md)
- Architecture overview: [`../../architecture.md`](../../architecture.md)
- C++ registry types: [`mclib/render_contract.h`](../../../mclib/render_contract.h), [`mclib/render_contract.cpp`](../../../mclib/render_contract.cpp)
- GLSL helpers: [`shaders/include/render_contract.hglsl`](../../../shaders/include/render_contract.hglsl)
- Grep census: [`scripts/check-render-contract-gbuffer1.sh`](../../../scripts/check-render-contract-gbuffer1.sh)
- FBO state machine: [`GameOS/gameos/gos_postprocess.cpp`](../../../GameOS/gameos/gos_postprocess.cpp) — `beginScene` (510), `enableMRT`/`disableMRT` (583-595), `runScreenShadow` (598+)
- Frame entry / clear site: [`GameOS/gameos/gameosmain.cpp`](../../../GameOS/gameos/gameosmain.cpp) — `pp->beginScene()` (405), `glClear` (409), sky-color clear (456)
- Terrain draw + `markTerrainDrawn`: [`GameOS/gameos/gameos_graphics.cpp`](../../../GameOS/gameos/gameos_graphics.cpp:2987-2998)
- The 5 confirmed-MRT-incomplete shaders: [`gos_vertex.frag`](../../../shaders/gos_vertex.frag), [`gos_vertex_lighted.frag`](../../../shaders/gos_vertex_lighted.frag), [`gos_tex_vertex.frag`](../../../shaders/gos_tex_vertex.frag), [`gos_tex_vertex_lighted.frag`](../../../shaders/gos_tex_vertex_lighted.frag), [`object_tex.frag`](../../../shaders/object_tex.frag)
- The reader: [`shadow_screen.frag:129`](../../../shaders/shadow_screen.frag) — `rc_pixelHandlesOwnShadow`
- Memory pointers (in `~/.claude/projects/A--Games-mc2-opengl-src/memory/`):
  - `stale_shader_cache_symptom.md`
  - `cull_gates_are_load_bearing.md`
  - `shadow_caster_eligibility_gate.md`
  - `debug_instrumentation_rule.md`

---

## Document history

- **2026-04-26 v1** — Initial spec (Option C-prime). Self-review then operator review surfaced two compounding flaws (alpha=1.0 clear, stale-GBuffer-under-overdraw). REJECTED.
- **2026-04-26 v2** — Reframed around coherence principle. Canary promoted to phase-0 prerequisite. Architecture forks on canary outcome (Option A or Option Hybrid). Awaiting review.
