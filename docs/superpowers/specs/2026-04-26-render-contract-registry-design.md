# Render Contract Registry — Design Spec

**Date:** 2026-04-26
**Status:** design (no code yet)
**Branch:** `claude/nifty-mendeleev`
**Predecessor work:** [`projectz-policy-split-report.md`](projectz-policy-split-report.md), [`render-contract.md`](../../render-contract.md), [`render-contract-and-shadows-roadmap.md`](../../plans/render-contract-and-shadows-roadmap.md)
**Scope:** audit + registry types + typed shader helpers. **No behavior change.** Predicate or pixel-output changes are out of scope; they belong to dedicated follow-up specs.

---

## TL;DR — Two Load-Bearing Discoveries

The renderer's G-buffer attachment 1 alpha channel is the load-bearing example of an implicit, drift-prone contract. The audit surfaced two findings that must not get buried:

> **Finding 1.** `GBuffer1.alpha` is **not a terrain flag.** It is a **post-process shadow mask.** `shadow_screen.frag` thresholds it at `> 0.5` to decide whether to skip its post-shadow application. Terrain is *one* producer; grass, terrain decals, and terrain overlays all opt in too — they are not terrain. The flag is misnamed in code and in shader comments.

> **Finding 2.** The contract is **already silently ambiguous.** `gos_terrain.frag` writes a continuous `materialAlpha` value into the same channel for water and shoreline pixels, while `shadow_screen.frag` thresholds it as a binary flag. Several additional fragment shaders appear to be drawn while MRT may be bound but do not declare `GBuffer1` — their attachment-1 alpha output is **driver-dependent**. Confirmed-vs-suspected status for those shaders is part of the phase-1 inventory (§3.2). Tiny material tweaks today can change shadow behavior tomorrow.

This spec names the contract, inventories every producer and reader, routes them through typed helpers that preserve current pixel output byte-identically, and adds a grep-enforced exit criterion that no shader writes the channel via a magic literal. **It does not decide the correct future semantics.** It preserves and names current semantics so later specs can change them safely. That is the same discipline that made the projectZ Policy Split safe.

---

## Core Thesis

The MC2 OpenGL renderer has an **implicit contract** about how each render path interacts with the G-buffer, shadow pipeline, depth/blend state, and post-process passes. The contract is partially documented in shader header comments, partially encoded in scattered `firstTextureAlpha` / `MC2_ISTERRAIN` / `alphaTestOn` checks, and partially undocumented driver-dependent behavior. Drift — intentional or accidental — surfaces as bugs in surprising places: concrete decals baked into terrain depth, decals floating without shadow, post-shadow darkening the wrong pixels, terrain-flag-overloaded-as-water-alpha, stale debug-overlay GL state breaking subsequent passes (the RAlt+P bug). The user's lived experience confirms it: "the alpha flag thing has bit us in the ass quite a few times."

The just-finished [projectZ Policy Split](projectz-policy-split-report.md) established a pattern: take an implicit/overloaded behavior, give it explicit named seams, and enforce the seams via the compiler. The render contract is the next-largest target for that treatment. This spec defines a **Render Contract Registry**: an audit + a typed C++/GLSL surface that every render path is routed through, designed so that future work (`ModernTerrainSurface`, overlay/decal unification, native-modern sidecars, post-shadow bridge reduction) cannot reintroduce the same drift silently.

The first phase is inert. The registry replaces magic constants with named helpers that compile to identical output. Behavior changes — including resolving the contradictions this spec surfaces — are deferred to dedicated follow-up specs.

This spec is an **audit + registry spec, not a cleanup spec**. It produces:
- a current-behavior table (what each shader/pass actually does today);
- an intended-contract table (what the registry says it should be called and how it is queried);
- a list of contradictions found in the audit;
- a phase-1 inert routing plan;
- follow-up tickets for the contradictions.

It does not fix shadow behavior, unify overlays with decals, change G-buffer semantics, or close the undefined-MRT-output gap. Those are blocked on this spec landing.

---

## Section 1 — Current Producers and Readers (audit)

### 1.1 Producers — fragment shaders writing GBuffer1

Five fragment shaders declare `layout(location=1) out vec4 GBuffer1` and write to it. All other fragment shaders declare only `location=0`; when they are drawn into an MRT-bound FBO their GBuffer1 output is **undefined** (driver-dependent). See section 3 for the MRT-incomplete inventory.

| Shader | Site | GBuffer1.rgb | GBuffer1.alpha | Effective meaning today |
|---|---|---|---|---|
| [`gos_terrain.frag`](../../../shaders/gos_terrain.frag) | 193, 232, 274, 322 (early-out / debug) | `(0.5, 0.5, 1.0)` flat-up | `1.0` | post-shadow skipped |
| `gos_terrain.frag` | 487 (main lit) | normal-encoded | `1.0` | post-shadow skipped |
| `gos_terrain.frag` | **533, 555, 591** (water/shoreline lit) | normal-encoded | **`materialAlpha` (continuous)** | **AMBIGUOUS — see §4.1** |
| [`decal.frag`](../../../shaders/decal.frag) | 67 | `(0.5, 0.5, 1.0)` flat-up | `1.0` | post-shadow skipped (decal opts in) |
| [`gos_grass.frag`](../../../shaders/gos_grass.frag) | 60 | bladeNormal encoded | `1.0` | post-shadow skipped (grass opts in) |
| [`terrain_overlay.frag`](../../../shaders/terrain_overlay.frag) | 55, 67, 75, 87 | `(0.5, 0.5, 1.0)` flat-up | `1.0` | post-shadow skipped (overlay opts in) |
| [`static_prop.frag`](../../../shaders/static_prop.frag) | 44, 54, 58, 59, 60, 61, 65 (debug) | `(0.0, 0.0, 1.0)` sentinel | `0.0` | post-shadow eligible |
| `static_prop.frag` | 74 (production) | normal-encoded | `0.0` | post-shadow eligible |

### 1.2 Reader

Single consumer: [`shadow_screen.frag:116`](../../../shaders/shadow_screen.frag).

```glsl
bool isTerrain = normalData.a > 0.5;
```

Used at line 148 to early-return (skip post-process shadow application). The local variable name `isTerrain` is the misnaming Finding 1 calls out: the predicate's actual semantic is "this pixel handled its own shadow (or doesn't want post-shadow); leave it alone."

### 1.3 FBO state machine

`gos_postprocess.cpp` switches between `glDrawBuffers(2, {COLOR0, COLOR1})` and `glDrawBuffers(1, {COLOR0})` at least 10 times per frame ([lines 319, 320, 523, 524, 586, 587, 594, 611, 768, 834, 867](../../../GameOS/gameos/gos_postprocess.cpp)). The contract on which mode applies during which pass is implicit in call ordering. A pass added in the wrong place silently renders with the wrong attachment count.

---

## Section 2 — Actual Semantic Meaning

The thing the channel actually means today is:

> **`GBuffer1.alpha > 0.5`** → `shadow_screen.frag` skips its post-process shadow multiply for this pixel ("self-shadow handled" / "post-shadow not wanted").
>
> **`GBuffer1.alpha <= 0.5`** → `shadow_screen.frag` applies post-process shadow normally ("screen-shadow eligible").

Producers opt in by writing `1.0`; producers opt out (i.e., choose to receive screen-shadow) by writing `0.0`. The current opt-in set is: terrain base, grass, terrain decals, terrain overlays. The current opt-out set is: static props. Everything else is undefined (see §3).

Naming the contract honestly:

- C++ name for the channel: `GBufferSlot::Normal_PostShadowMask`.
- GLSL helper names use `shadowHandled` / `screenShadowEligible`.
- The reader-side helper is named `rc_pixelHandlesOwnShadow`, not `rc_pixelIsTerrain`.
- The legacy continuous-alpha case (water/shoreline) gets an **intentionally ugly** helper name: `rc_gbuffer1_legacyTerrainMaterialAlpha`. The ugly name is the point — it announces "this is preserved drift, not clean design."

---

## Section 3 — Known Ambiguities (audit findings, not fixed in this spec)

### 3.1 `GBuffer1.alpha` is overloaded as both post-shadow mask and material alpha

Lines 533, 555, 591 of [`gos_terrain.frag`](../../../shaders/gos_terrain.frag) write `vec4(N*0.5+0.5, materialAlpha)` for water and shoreline pixels. `materialAlpha` is a continuous value driven by water depth and shoreline blend factors. `shadow_screen.frag` reads it via `> 0.5`. This produces an **accidental third state**:

| `materialAlpha` value | shadow_screen.frag classification | Behavior |
|---|---|---|
| `0.0` | not handled → applies post-shadow | water gets darkened |
| `1.0` | handled → skips post-shadow | water acts like terrain |
| `0.0–0.5` (open) | not handled | inconsistent with adjacent water of same surface |
| `0.5–1.0` (open) | handled | inconsistent with adjacent water of same surface |

**Status:** CONTRACT VIOLATION / AMBIGUOUS PRODUCTION BEHAVIOR. **Not fixed in this spec.** The phase-1 helper (`rc_gbuffer1_legacyTerrainMaterialAlpha`) preserves the byte-identical literal; the ugly name surfaces the ambiguity to every future reader. Resolution is follow-up F1.

### 3.2 (Finding 2b) MRT-incomplete shaders — undefined attachment-1 output

Six fragment shaders declare only `location=0` and are confirmed or suspected to be drawn while MRT may be bound. Their `GBuffer1` output is **driver-dependent** (typical: leaves the previous frame's value, or whatever the implementation chose to clear/preserve). On the developer's AMD/Windows configuration today, the result is acceptable; a driver update, GPU swap, or clear-policy tweak could flip shadow behavior on those pixels silently.

#### MRT-incomplete inventory

The "Drawn while MRT bound?" column is the key uncertainty axis. Phase-1 commit 7 must verify each row by cross-referencing C++ pass-identity markers against the FBO state at draw time. Until then, the table reflects audit-time best knowledge.

**Confirmed MRT-bound, no `GBuffer1` declaration:**

| Shader | Writes Color0? | Writes GBuffer1? | Implied current behavior | Required contract decision |
|---|---|---|---|---|
| [`gos_tex_vertex.frag`](../../../shaders/gos_tex_vertex.frag) — IS_OVERLAY bridge (Bucket D2 in `render-contract.md`) | yes | **no** | undefined attachment-1 alpha | F3 |

**Suspected MRT-bound (likely, not yet verified):**

| Shader | Writes Color0? | Writes GBuffer1? | Implied current behavior | Required contract decision |
|---|---|---|---|---|
| [`gos_vertex.frag`](../../../shaders/gos_vertex.frag) (untextured object pass) | yes | **no** | undefined if MRT live | F3 |
| [`gos_vertex_lighted.frag`](../../../shaders/gos_vertex_lighted.frag) | yes | **no** | undefined if MRT live | F3 |
| [`gos_tex_vertex_lighted.frag`](../../../shaders/gos_tex_vertex_lighted.frag) | yes | **no** | undefined if MRT live | F3 |
| [`object_tex.frag`](../../../shaders/object_tex.frag) | yes | **no** | undefined if MRT live | F3 |

**Unconfirmed / timing-dependent:**

| Shader | Writes Color0? | Writes GBuffer1? | Implied current behavior | Required contract decision |
|---|---|---|---|---|
| [`gos_text.frag`](../../../shaders/gos_text.frag) | yes | **no** | undefined if MRT live; HUD/text pass may run after MRT unbind | F3 |

Step 7 of the commit sequence is responsible for confirming "Drawn while MRT bound?" for each row by cross-referencing the C++ pass-identity markers against the FBO state at draw time. The spec does **not** add `GBuffer1` writes to these shaders. It documents the gap and produces the inventory.

Three viable resolutions for the follow-up F3 spec, in increasing intrusiveness:

1. Add explicit `GBuffer1` declaration + `rc_gbuffer1_screenShadowEligible(...)` write to every shader bound during MRT. Most explicit; touches the most files.
2. Route those draws through a non-MRT pass (drawn before MRT bind, or rebound to a single-attachment FBO).
3. Use `glColorMask` or `glDrawBuffers(1, {COLOR0})` toggles to disable the missing attachment per-draw. Lowest source touch; relies on the FBO state machine to do the right thing.

The choice is data-driven on whether any of these shaders' pixels are subsequently sampled by `shadow_screen.frag` (open question 1). Decision deferred.

### 3.3 (Finding 1 corollary) Decals/grass/overlays opt in by claiming to be terrain

[`decal.frag:67`](../../../shaders/decal.frag) writes `1.0` with the comment "Mark as terrain to exclude from deferred shadow multiply." Grass and terrain overlays do the same. The comment is honest about the *mechanism* and wrong about the *semantic*: these passes are not terrain, they want to opt out of post-shadow because they receive shadow elsewhere (forward-shaded inline) and would otherwise be double-shadowed.

**Resolution:** rename the contract's *meaning* (Finding 1) without changing the bit. Phase 1 introduces helpers named `rc_gbuffer1_shadowHandled(N)` (writes `1.0`) and `rc_gbuffer1_screenShadowEligible(N)` (writes `0.0`). Decal/grass/overlay shaders call the former with no pretense of being terrain.

### 3.4 Shadow-eligibility gate is open-coded at every caller

Implicit gate at [`mclib/tgl.cpp:2700-2730`](../../../mclib/tgl.cpp) and surrounding `TG_Shape::Render` paths. Inputs include `gosCanRenderShape()`, `!alphaTestOn`, `appearance->castShadow()`, and (formerly) `!firstTextureAlpha` removed per user-memory `shadow_caster_eligibility_gate.md` after the 743efd6 misdiagnosis. No single named function answers "should this shape cast a dynamic shadow?"

**Status:** Phase 1 documents the conjunction in `ShadowContract` as a per-`PassIdentity` table that *mirrors* what each callsite computes today. Phase 1 does **not** centralize the computation; the `firstTextureAlpha` lesson is exactly why "cleaning up" too early is dangerous. Centralization is follow-up F4.

### 3.5 Debug overlays do not have a state contract

The RAlt+P projectZ overlay bug (deferred ticket E in the projectZ closing report) is a debug overlay leaking depth/blend state into following passes. Other debug overlays (RAlt+F1 bloom, F2 shadow debug, Tracy on-screen visualizer, gos_text HUD) have similar exposure but no stated contract. **Phase 1** introduces `PassIdentity::DebugOverlay` and a `PassStateContract` profile (depth disabled, blend explicit, GL state captured/restored, off by default, no gameplay feedback) as **descriptive types only** — not enforced. Routing existing overlays and the RAlt+P fix are follow-up F5.

---

## Section 4 — Registry Design

The registry is **two surfaces** that mirror each other: a C++ side and a GLSL side. Neither side changes behavior in phase 1.

### 4.1 C++ side

A new header `mclib/render_contract.h` (small, header-only, included by anyone who issues draws or sets up materials):

```cpp
namespace render_contract {

// The kind of draw call. Queryable from material setup, shadow paths,
// FBO state machine. Not load-bearing in phase 1; serves to make pass
// identity an explicit value rather than an inferred one.
enum class PassIdentity : uint8_t {
    Unknown = 0,            // legacy / not yet classified — phase-1 default
    TerrainBase,            // tessellated heightfield (gos_terrain.frag)
    TerrainOverlay,         // perimeter cement, transitions (terrain_overlay.frag)
    TerrainDecal,           // craters, footprints, scorch marks (decal.frag)
    Grass,                  // GPU grass (gos_grass.frag) — terrain-derived
    Water,                  // water surface + detail (intentional projected, see render-contract.md B1)
    OpaqueObject,           // mechs, vehicles, buildings — opaque pass
    AlphaObject,            // same shapes when alpha-tested or alpha-blended
    StaticProp,             // GPU static-prop renderer (static_prop.frag)
    ParticleEffect,         // weapon bolts, weather, clouds, explosions
    UI,                     // HUD, text, menu (screen-space)
    DebugOverlay,           // F1/F2/RAlt+P/etc. — diagnostic only, never gameplay
    ShadowCaster,           // depth-only pass for static terrain or dynamic mech shadow map
    PostProcess,            // fullscreen post passes (shadow_screen, ssao, bloom, godray, shoreline)
};

// What each G-buffer attachment slot carries. The alpha of slot 1 is
// renamed honestly: it is a post-process shadow mask, not a terrain flag.
// Legacy code/comments may still call it "terrain flag"; the registry does
// not preserve that misleading name as the primary API.
enum class GBufferSlot : uint8_t {
    Color0_Albedo = 0,
    Normal1_PostShadowMask = 1,   // RGB = normal*0.5+0.5; alpha > 0.5 → skip post-shadow
};

// A pass's shadow-pipeline relationship. Computed once per material
// setup (phase 2+); for phase 1, mirrors the existing conjunction
// at each callsite without changing it.
struct ShadowContract {
    bool castsStaticShadow;        // included in static terrain shadow atlas
    bool castsDynamicShadow;       // included in dynamic local shadow map
    bool skipsPostScreenShadow;    // shadow_screen.frag will not darken this pixel
                                   // Implemented today by writing GBuffer1.a > 0.5;
                                   // the storage mechanism is documented separately
                                   // and may evolve under follow-up F1.
};

// A pass's GL-state contract. Documents what the pass requires on entry
// and guarantees on exit. Phase 1: descriptive struct only, not enforced.
// Phase 2 (separate spec): debug-build assertions.
struct PassStateContract {
    bool requiresDepthTest;
    bool requiresDepthWrite;
    enum class BlendMode { Opaque, AlphaBlend, AlphaTest, Additive } blend;
    bool requiresMRT;             // expects glDrawBuffers(2, {COLOR0, COLOR1})
    const char* expectedFBO;      // documentary string ("scene HDR FBO", "shadow atlas FBO", ...)
    bool restoresStateOnExit;     // overlay must restore; main scene passes need not
};

// The registry: a compile-time-constructible table, one row per PassIdentity.
// Lives in render_contract.cpp; queried by name.
const ShadowContract&    shadowContractFor(PassIdentity);
const PassStateContract& stateContractFor(PassIdentity);
const char*              passIdentityName(PassIdentity);

} // namespace render_contract
```

The enums are **inert in phase 1**. They describe state, not change it. The two `*ContractFor` accessors return values that mirror what each callsite currently does. A future spec may centralize the conjunctions (e.g., make `TG_Shape::Render`'s eligibility check a single call); this spec only documents and routes.

### 4.2 GLSL side

A new include `shaders/include/render_contract.hglsl`:

```glsl
// Render Contract Registry — GLSL helpers.
//
// GBuffer1 (color attachment 1) is RGBA16F.
//   .rgb = normal-encoded (n * 0.5 + 0.5)
//   .a   = post-process shadow mask:
//          a > 0.5  → shadow_screen.frag skips this pixel (self-shadow handled)
//          a <= 0.5 → shadow_screen.frag applies post-shadow normally
//
// Every fragment shader that writes attachment 1 must use one of these.
// No shader may write `GBuffer1 = vec4(...)` directly; a grep census enforces.

#ifndef RENDER_CONTRACT_HGLSL
#define RENDER_CONTRACT_HGLSL

// Pixel handles its own shadow inside this shader. Used by terrain base,
// grass, terrain overlays, terrain decals.
PREC vec4 rc_gbuffer1_shadowHandled(PREC vec3 normal) {
    return vec4(normal * 0.5 + 0.5, 1.0);
}

// Pixel is eligible for screen-space shadow application by shadow_screen.frag.
// Used by static props and any future world-space object renderer with a
// G-buffer write.
PREC vec4 rc_gbuffer1_screenShadowEligible(PREC vec3 normal) {
    return vec4(normal * 0.5 + 0.5, 0.0);
}

// PRE-ENCODED-NORMAL VARIANTS — same semantic, signature matches existing
// callsites that emit a literal (0.5, 0.5, 1.0) without computing a normal.
PREC vec4 rc_gbuffer1_shadowHandled_flatUp() {
    return vec4(0.5, 0.5, 1.0, 1.0);
}

// LEGACY DEBUG SENTINEL (intentionally legacy-named). Used only by
// static_prop.frag debug paths that emit a sentinel color/normal pair without
// real lighting. Not a recommended pattern for new shaders; preserved here so
// the debug paths route through the registry rather than writing literals.
PREC vec4 rc_gbuffer1_legacyDebugSentinelScreenShadowEligible() {
    return vec4(0.0, 0.0, 1.0, 0.0);
}

// LEGACY ESCAPE HATCH (intentionally ugly name). Used only by gos_terrain.frag
// water/shoreline paths (lines 533, 555, 591) that today write a CONTINUOUS
// materialAlpha into the post-shadow MASK channel. The mask is supposed to be
// binary; a continuous value here is an undocumented overload that flips
// shadow behavior based on water depth / shoreline blend. See spec section 3.1.
//
// DO NOT use this helper in any other shader. New callsites must use
// rc_gbuffer1_shadowHandled or rc_gbuffer1_screenShadowEligible.
//
// This helper exists only to preserve byte-identical pixel output through the
// inert phase. Resolution is follow-up F1 (split material alpha from shadow
// mask, possibly via bit-packing or a dedicated channel).
PREC vec4 rc_gbuffer1_legacyTerrainMaterialAlpha(PREC vec3 normal,
                                                  PREC float materialAlpha) {
    return vec4(normal * 0.5 + 0.5, materialAlpha);
}

// Reader side — used by shadow_screen.frag and any future post-process pass
// that gates on the mask.
//
// CANONICAL THRESHOLD DEFINITION. After commit 6, shadow_screen.frag must not
// directly threshold GBuffer1.a. This function is the sole canonical definition
// of the post-shadow mask threshold; any future change to the threshold value
// or storage encoding (e.g. follow-up F1 bit-packing) modifies this function
// and nothing else.
bool rc_pixelHandlesOwnShadow(PREC vec4 gbuffer1) {
    return gbuffer1.a > 0.5;
}

#endif
```

Every helper compiles to exactly the same fragment-shader output as the literal it replaces. The point is **named seams**, not behavior. The legacy helper's ugly name is the point: it announces "this is preserved drift, not clean design" to every future reader.

### 4.3 Source markers (grep-friendly)

Mirror the projectZ split's `// [PROJECTZ:Category id=...]` discipline. C++ callsites that issue draws within a known pass identity get:

```cpp
// [RENDER_CONTRACT:Pass=TerrainBase id=quad_render_main]
```

Shader files get a header block:

```glsl
// [RENDER_CONTRACT]
//   Pass:            TerrainOverlay
//   Color0:          RGBA color, alpha-blended
//   GBuffer1.alpha:  rc_gbuffer1_shadowHandled (post-shadow skipped)
//   ShadowContract:  castsStatic=false, castsDynamic=false,
//                    skipsPostScreenShadow=true
//   StateContract:   depthTest=true, depthWrite=false, blend=AlphaBlend,
//                    requiresMRT=true, restoresStateOnExit=false
```

Markers are documentation, not load-bearing. They make the contract greppable across the codebase and across the GLSL/C++ boundary.

---

## Section 5 — Current vs Intended Routing Table

This is the master routing table for phase 1. **Current alpha** must equal **Intended helper output** byte-for-byte; any disagreement is a bug in the routing, not a fix.

| Shader | Current alpha write | Current meaning | Phase-1 helper | Notes |
|---|---|---|---|---|
| `gos_terrain.frag` early-out (193, 232, 274, 322) | `1.0` (literal `vec4(0.5,0.5,1.0,1.0)`) | terrain self-shadows | `rc_gbuffer1_shadowHandled_flatUp()` | byte-identical |
| `gos_terrain.frag` main lit (487) | `1.0` | terrain self-shadows | `rc_gbuffer1_shadowHandled(N)` | byte-identical |
| `gos_terrain.frag` water/shore (533, 555, 591) | `materialAlpha` (continuous) | **AMBIGUOUS** §3.1 | `rc_gbuffer1_legacyTerrainMaterialAlpha(N, materialAlpha)` | byte-identical; ugly helper name flags drift |
| `terrain_overlay.frag` (55, 67, 75, 87) | `1.0` | overlay opts into self-shadow | `rc_gbuffer1_shadowHandled_flatUp()` | byte-identical |
| `decal.frag` (67) | `1.0` | decal opts into self-shadow | `rc_gbuffer1_shadowHandled_flatUp()` | byte-identical; rename "Mark as terrain" comment to honest semantic |
| `gos_grass.frag` (60) | `1.0` | grass opts into self-shadow | `rc_gbuffer1_shadowHandled(bladeNormal)` | byte-identical |
| `static_prop.frag` debug (44, 54, 58–65) | `0.0` (sentinel `vec4(0,0,1,0)`) | post-shadow eligible (debug) | `rc_gbuffer1_legacyDebugSentinelScreenShadowEligible()` | byte-identical |
| `static_prop.frag` production (74) | `0.0` | post-shadow eligible | `rc_gbuffer1_screenShadowEligible(normalize(v_normal))` | byte-identical |
| `gos_vertex.frag`, `gos_vertex_lighted.frag`, `gos_tex_vertex.frag`, `gos_tex_vertex_lighted.frag`, `object_tex.frag`, `gos_text.frag` | **undefined** | driver-dependent §3.2 | **none — inventory only** | F3 follow-up |
| `shadow_screen.frag:116` (reader) | reads `> 0.5` | thresholds mask | `rc_pixelHandlesOwnShadow(normalData)` | byte-identical; rename `isTerrain` → `pixelHandlesOwnShadow` |

---

## Section 6 — Frozen Surfaces (do not touch in this spec's commits)

If a phase-1 commit appears to require touching any of these, **stop and flag**. The change has exited containment scope.

- All fragment-shader pixel output (must be byte-identical pre/post)
- `MC2_ISTERRAIN` material flag and its consumers
- Shadow caster eligibility conjunction at `tgl.cpp:2700-2730` and at every shape-render callsite
- FBO setup and `glDrawBuffers` call ordering in `gos_postprocess.cpp`
- shadow_screen.frag's `> 0.5` threshold value
- `gos_terrain.frag` water/shoreline `materialAlpha` writes (§3.1) — preserved by ugly legacy helper
- The `firstTextureAlpha` conditional (already removed per user-memory note; do not reintroduce)
- The 8 projectZ wrappers in `mclib/camera.h` (load-bearing post-projectZ-split)
- Water rendering (Bucket B1 in `render-contract.md` — projected exception)
- The RAlt+P overlay bug (independent ticket; F5 instance)
- The undefined-MRT-output six (§3.2) — inventory only, no shader bodies edited

---

## Section 7 — Surfaces That May Continue in Parallel

These are not blocked by this spec landing:

- The RAlt+P overlay GL-state fix (resolves an existing F5 instance but is its own ticket)
- ModernTerrainSurface planning (advisor's "B" — separate spec)
- Overlay/decal unification planning (advisor's "C" — separate spec)
- Native-modern sidecars planning (advisor's "D" — separate spec)
- Documentation-only edits to `render-contract.md` (the existing submission-space contract doc)

---

## Section 8 — Inert Implementation Phase (Commit Sequence)

Each commit is reviewable in isolation, can be reverted independently, and changes no rendered pixel. **Smoke-tier1 5/5 PASS gate between commits.** Build performed in the `nifty-mendeleev` worktree per CLAUDE.md (RelWithDebInfo, no `cp -r` on deploy).

### Commit 1 — `render-contract: add registry header (no callsite changes)`

- Create `mclib/render_contract.h` with `PassIdentity`, `GBufferSlot`, `ShadowContract`, `PassStateContract` enums/structs and accessor function declarations.
- Create `mclib/render_contract.cpp` with conservative entries for confirmed passes. Rows whose current contract is not fully audited at commit-1 time **must** be entered as `Unknown` / `TODO_RENDER_CONTRACT`, not guessed. This is the render-contract analogue of projectZ's `ROUTING_REVIEW_REQUIRED`. Authoritative entries are filled in as commits 7 (markers) and 9 (closing report) confirm them.
- No callsite changes. No header included by any other production file yet.
- **Validation:** project compiles in nifty. Smoke-tier1 5/5 PASS. Pixel-diff zero (header is unused; impossible to alter output).

### Commit 2 — `render-contract: add typed GLSL helpers (shader bodies unchanged)`

- Create `shaders/include/render_contract.hglsl` with all helpers defined in §4.2.
- Do **not** include the header in any shader yet.
- **Validation:** smoke-tier1 5/5 PASS. Pixel-diff zero.

### Commit 3 — `render-contract: route gos_terrain.frag GBuffer1 writes through helpers`

- Add `#include "include/render_contract.hglsl"` at top of `gos_terrain.frag`.
- Replace the 8 `GBuffer1 = vec4(...)` literals per the §5 routing table.
- Add `// [RENDER_CONTRACT]` header block to the shader.
- **Validation:** smoke-tier1 5/5 PASS. Visual spot-check on `mc2_01` (terrain shading), one mission with water (`mc2_03`), one mission with shoreline. Pixel-diff zero on the no-water mission. Last-bit FP drift on water is acceptable; anything beyond last-bit halts the commit and is investigated.

### Commit 4 — `render-contract: route decal/grass/overlay GBuffer1 writes through helpers`

- Same treatment for `decal.frag`, `gos_grass.frag`, `terrain_overlay.frag`.
- Update the `decal.frag` comment from "Mark as terrain to exclude from deferred shadow multiply" to "Decal handles its own shadow; opt out of post-process shadow multiply" (Finding 1 honesty).
- Add `// [RENDER_CONTRACT]` header block to each.
- **Validation:** smoke-tier1 5/5 PASS. Pixel-diff zero.

### Commit 5 — `render-contract: route static_prop.frag GBuffer1 writes through helpers`

- Same treatment for `static_prop.frag` (one production path, six debug paths).
- Add `// [RENDER_CONTRACT]` header block.
- **Validation:** smoke-tier1 5/5 PASS. Pixel-diff zero.

### Commit 6 — `render-contract: route shadow_screen.frag reader through helper`

- Add `#include "include/render_contract.hglsl"` to `shadow_screen.frag`.
- Replace `bool isTerrain = normalData.a > 0.5;` (line 116) with `bool pixelHandlesOwnShadow = rc_pixelHandlesOwnShadow(normalData);`.
- Rename uses at lines 121, 148 (rename only, no logic change).
- Add `// [RENDER_CONTRACT]` header block.
- **Validation:** smoke-tier1 5/5 PASS. Pixel-diff zero.

### Commit 7 — `render-contract: tag C++ draw callsites with PassIdentity markers + MRT-incomplete inventory`

- Add `// [RENDER_CONTRACT:Pass=...]` markers at the top of major draw functions:
  - Terrain submission paths in `quad.cpp` and `terrain.cpp` → `Pass=TerrainBase`
  - `crater.cpp` decal submission → `Pass=TerrainDecal`
  - Static prop renderer in `gameos_graphics.cpp` → `Pass=StaticProp`
  - `TG_Shape::Render` paths → `Pass=OpaqueObject` / `AlphaObject`
  - `gosFX` paths → `Pass=ParticleEffect`
  - HUD/UI submission → `Pass=UI`
  - Debug overlay submission (RAlt+P, F1, F2) → `Pass=DebugOverlay`
- Inventory document committed alongside under `docs/superpowers/specs/render-contract-callsite-inventory.md`. The inventory **must** confirm or refute the "Drawn while MRT bound?" column for each row in the §3.2 MRT-incomplete table; the answer drives F3 prioritization.
- **No code logic change.**
- **Validation:** smoke-tier1 5/5 PASS. Pixel-diff zero.

### Commit 8 — `render-contract: deprecate raw GBuffer1 literal writes (grep census)`

- Add `scripts/check-render-contract-gbuffer1.sh` enforcing the following invariant:
  > **Fail** if any production fragment shader assigns `GBuffer1` directly from `vec4(...)` instead of an `rc_*` helper. The implementation script must ignore comments and exclude `shaders/include/render_contract.hglsl` helper definitions.
  
  The exact script form (grep, perl, or python) is decided at implementation time after testing against current source forms (with and without spaces, with and without comments on the same line, across all `shaders/*.frag` and any nested include paths). The `[RENDER_CONTRACT` literal contains a `[` that needs regex escaping or fixed-string mode; the impl must handle this correctly.
- Run the script in the smoke-tier1 wrapper.
- Cross-reference the registry from `docs/render-contract.md` (existing doc).
- This is the equivalent of the projectZ split's compiler-enforced exit criterion: a grep census instead of a `[[deprecated]]` warning, because GLSL has no compiler-deprecation primitive.
- **Validation:** smoke-tier1 5/5 PASS. Census script reports zero violations.

### Commit 9 — `render-contract: closing report + follow-up ticket inventory`

- Write `docs/superpowers/specs/render-contract-registry-report.md` modeled on [`projectz-policy-split-report.md`](projectz-policy-split-report.md):
  - Per-commit smoke status
  - Routing distribution (which shaders / how many sites per helper)
  - Surprises encountered
  - Deferred operator-side checks (golden-frame diff if not done in-band)
  - Confirmed exit criteria
  - Follow-up tickets F1–F8 promoted to spec ideas
- This commit is the spec's terminal state.

---

## Section 9 — Exit Criteria

The spec is complete when **all** of the following hold:

1. `mclib/render_contract.h` and `.cpp` exist with the enums, structs, and table per §4.1.
2. `shaders/include/render_contract.hglsl` exists with all helpers per §4.2.
3. Every `GBuffer1 = vec4(...)` literal in `shaders/*.frag` is replaced with an `rc_*` helper call. Grep census reports zero violations.
4. `shadow_screen.frag` reads through `rc_pixelHandlesOwnShadow`.
5. Every shader that writes `GBuffer1` carries a `// [RENDER_CONTRACT]` header block.
6. Every major C++ draw entry point carries a `// [RENDER_CONTRACT:Pass=...]` marker (inventory document committed).
7. MRT-incomplete inventory in §3.2 has its "Drawn while MRT bound?" column verified by audit (commit 7).
8. Smoke-tier1 5/5 PASS after each of the 9 commits, build performed in nifty worktree.
9. **Pixel-diff policy.** Expected: zero pixel diff on `mc2_01` start / +30s / +60s for commits 3, 4, 5, 6 (the four that touch shader bodies). Any non-zero diff blocks the commit until investigated. A diff may be accepted only if proven non-visible / non-semantic (e.g., shader-compiler expression-grouping change, FMA selection difference) and documented in the closing report. Visible or threshold-affecting differences halt the commit unconditionally.
10. **Legacy escape-hatch census.** `rc_gbuffer1_legacyTerrainMaterialAlpha` appears only in `gos_terrain.frag` at the three audited water/shoreline sites (533, 555, 591). `rc_gbuffer1_legacyDebugSentinelScreenShadowEligible` appears only in `static_prop.frag` debug paths (44, 54, 58, 59, 60, 61, 65). Either helper appearing elsewhere is a contract violation; the grep census enforces.
11. Closing report committed.
12. Follow-up tickets F1–F6+ captured (in the report; not necessarily resolved).

The compiler enforces nothing in phase 1. The grep census enforces magic literals are gone. Behavior must be byte-identical for non-water surfaces and last-bit-equivalent for water.

---

## Section 10 — Non-Goals (Explicit)

- **Resolving §3.1 (terrain-flag vs. material-alpha overload).** Phase-1 helper preserves byte-identically.
- **Renaming the stored bit / bit-packing the mask separate from material alpha.** `shadow_screen.frag`'s `> 0.5` stays.
- **Centralizing the shadow-eligibility conjunction** at `TG_Shape::Render`. Phase 1 documents only.
- **Closing the undefined-MRT-output gap (§3.2).** Phase 1 inventories only; no shader bodies edited.
- **Fixing the RAlt+P overlay state bug.** Independent ticket.
- **Any `glDrawBuffers` call-site change.**
- **Any change to `MC2_ISTERRAIN` semantics.**
- **Runtime debug-build GPU-readback validation** (the deferred option from the brainstorm). Useful in a follow-up.
- **`ModernTerrainSurface`, overlay/decal unification, native-modern sidecars.** Each is its own spec, sequenced after this one lands.
- **Deciding correct future semantics.** The spec preserves and names current semantics; later specs decide change.

---

## Section 11 — Open Questions

1. **For each MRT-incomplete shader in §3.2, are its pixels actually sampled by `shadow_screen.frag`?** Answered by commit-7 inventory work. If yes, F3 escalates from "latent risk" to "active driver-dependency bug." If no, F3 stays latent.

2. **Is `ShadowCaster` a `PassIdentity` or a separate axis?** Static-shadow and dynamic-shadow passes draw the same shapes through different programs. The shape's `PassIdentity` (`OpaqueObject`) is invariant; the *pass role* is `ShadowCaster`. May need a second axis (`PassRole` orthogonal to `PassIdentity`). Defer until the registry is in use.

3. **Should the GLSL helper file be auto-included by the build, or explicit `#include` per shader?** Recommend explicit per shader — matches projectZ-split discipline.

4. **What is the precise grep-census regex?** Tested against current source forms (with and without spaces) before commit 8 lands. The script in §8/commit 8 is a starting point.

5. **Stable IDs on markers?** projectZ needed them because diagnostics emitted them. This spec has no diagnostic emitter in phase 1. Recommend: include IDs only on C++ markers (`id=quad_render_main`); skip IDs on shader-header markers (one block per shader, no ID needed).

6. **Should the per-shader header block be redundant with per-helper-call routing?** Both serve different purposes (header documents intent; helper documents implementation). Keep both in phase 1; consolidate if maintenance proves drift.

---

## Section 12 — Follow-Up Tickets (out of scope)

- **F1.** (§3.1) Resolve terrain-flag vs. material-alpha overload for water/shoreline. Bit-pack the mask separately, or add a dedicated material-alpha channel, or document the threshold-as-policy. Decision-input: whether water is supposed to receive post-process shadow.
- **F2.** (§3.3 / Finding 1) Complete remaining legacy terminology cleanup outside the phase-1 touched lines: old comments, docs, C++ identifiers, debug labels, and any remaining "terrain flag" wording. Phase 1 already renames the `decal.frag` comment (commit 4) and the `shadow_screen.frag` reader local (commit 6); F2 covers everything else.
- **F3.** (§3.2 / Finding 2b) Close the undefined-MRT-output gap. Choose among (a) declare GBuffer1 in every MRT-bound shader, (b) route undefined shaders through a non-MRT pass, (c) toggle `glDrawBuffers` per-draw. Decision driven by commit-7 inventory.
- **F4.** (§3.4) Centralize shadow-eligibility conjunction. Move the open-coded conjunction from per-callsite to a single `shadowContractFor(passIdentity).castsDynamicShadow` query. Must reference user-memory note about commit 743efd6 and preserve the conjunction byte-identically.
- **F5.** (§3.5) Define `DebugOverlayPass` enforcement and route existing overlays. Includes the RAlt+P fix as one instance.
- **F6.** Native-modern sidecars (advisor's D from post-projectZ roadmap) — separate spec.
- **F7.** ModernTerrainSurface / LegacyTerrainImport (advisor's B) — separate spec.
- **F8.** Overlay/decal unification (advisor's C) — separate spec; gated on F1/F2.
- **F9.** Optional runtime GPU-readback validation in debug builds (env-gated, like `MC2_PROJECTZ_TRACE`). Useful once the contract is named; out of scope for naming pass.

---

## Why This First, Why This Way

The projectZ split proved a pattern: implicit overloaded behavior → explicit named seams → enforcement → behavior change in a *separate* spec. The render contract is the next-largest implicit-overload in the renderer, with documented incidents (743efd6, the RAlt+P overlay bug, the silent-trinary water alpha) to motivate naming it before any further refactor.

The phase-1 inert constraint matters for the same reason it mattered for projectZ: every prior attempt to "clean up" terrain admission introduced regressions because cleanup conflated naming with behavior change. The registry must establish the seams *first*, give them grep-enforceable identity, and only then enable a future spec to change what flows through them.

Doing this before `ModernTerrainSurface` or overlay/decal unification means those refactors land into a renderer with explicit pass identity, explicit shadow contracts, and explicit G-buffer semantics. Doing it after means each refactor reintroduces drift opportunities while the contract is still implicit.

The core discipline: **name what is, before changing what should be.**

---

## References

- [`projectz-policy-split-report.md`](projectz-policy-split-report.md) — template for spec discipline, exit criteria, closing report
- [`2026-04-25-projectz-containment-design.md`](2026-04-25-projectz-containment-design.md) — template for "containment + measurement, no behavior change" framing
- [`render-contract.md`](../../render-contract.md) — existing submission-space contract (orthogonal; this spec extends, doesn't replace)
- [`render-contract-and-shadows-roadmap.md`](../../plans/render-contract-and-shadows-roadmap.md) — phase sequencing for shadow stability work
- [`amd-driver-rules.md`](../../amd-driver-rules.md) — driver-quirk constraints relevant to MRT/usampler2D issues
- [`architecture.md`](../../architecture.md) — render pipeline overview, shadow pipeline, coordinate spaces
- User auto-memory: `shadow_caster_eligibility_gate.md` — 743efd6 misdiagnosis, motivates §3.4 / F4
- User auto-memory: `cull_gates_are_load_bearing.md` — discipline pattern for load-bearing implicit gates
- User auto-memory: `debug_instrumentation_rule.md` — env-gated instrumentation conventions (relevant to F9)
- [`mclib/tgl.cpp:2700-2730`](../../../mclib/tgl.cpp) — current shadow eligibility gate
- [`shaders/shadow_screen.frag:116`](../../../shaders/shadow_screen.frag) — sole `GBuffer1.alpha` consumer
- [`GameOS/gameos/gos_postprocess.cpp:309-320`](../../../GameOS/gameos/gos_postprocess.cpp) — MRT FBO setup; `glDrawBuffers` switching
- All shader files writing GBuffer1: [`gos_terrain.frag`](../../../shaders/gos_terrain.frag), [`decal.frag`](../../../shaders/decal.frag), [`gos_grass.frag`](../../../shaders/gos_grass.frag), [`terrain_overlay.frag`](../../../shaders/terrain_overlay.frag), [`static_prop.frag`](../../../shaders/static_prop.frag)
- All MRT-incomplete shaders (§3.2): `gos_vertex.frag`, `gos_vertex_lighted.frag`, `gos_tex_vertex.frag`, `gos_tex_vertex_lighted.frag`, `object_tex.frag`, `gos_text.frag`
