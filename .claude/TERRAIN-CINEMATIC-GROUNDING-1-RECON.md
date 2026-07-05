# TERRAIN-CINEMATIC-GROUNDING-1 — RECON (read-only)

Worktree: `A:/Games/mc2-controlmap-sample-1` @ `380910d1` (HEAD as checked). No source changes / no build / no launch. Advisor slice from the mc2_17 cutscene report: "mechs look like actors pasted on a stage floor; shadows harsh + more detailed than ground; lots of black without readable detail."

Goal families: (a) contact/blob shadow under units, (b) dust/scorch/footprint decal, (c) soften/clamp harsh projected shadows, (d) cutscene lighting preset / ambient lift (black crush).

---

## 1. CURRENT-STATE INVENTORY (file:line)

### Unit shadows (the "harsh" source)
- **Mech dynamic shadow = screen-space post-process**, NOT a projected quad. `shaders/shadow_screen.frag:320-326` — for non-static-prop object pixels, `sampleDynamicShadow(worldPos, objN, mechSoft, 0.55)` combined via `min()` into a fullscreen multiplicative shadow factor. Terrain/grass/decals early-out (`pixelHandlesOwnShadow`, :291) and shadow themselves inline.
- **Shadow floor is a hard clamp** — the darkest a shadow gets:
  - terrain / static `sampleShadowMap`: `return mix(0.4, 1.0, shadow)` (`shadow_screen.frag:143`; also `shadow.hglsl:77`, `:283`) → fully-shadowed = 0.40 (60% darken).
  - mech `sampleDynamicShadow`: `return mix(shadowFloor, 1.0, shadow)` with `shadowFloor=0.55` passed from :324 → mech self-shadow already raised to 0.55 (subtler). Legacy non-CSM path remaps through the same 0.4 floor (`shadow_screen.frag:228`).
- **"Harsh" root cause** is NOT resolution or missing penumbra — PCF is 8-tap Poisson, gradient-adaptive, terminator-smoothstepped, normal-offset-biased (`shadow_screen.frag:154-235`, `shadow.hglsl:33-78`). It is (i) the **fixed 0.40 floor** darker than typical shadowed-terrain readable detail, and (ii) the mech's cast shadow onto terrain uses the *same* dynamic map at the same floor → hard edge against a low-contrast crushed ground. `SHADOW-SHARPEN` comments (`shadow.hglsl:64` `mix(3.2->2.4)`) show softness was *deliberately reduced* for mechs — good for tactical, too hard for a close cutscene.
- **Softness is a live tunable**: `shadowSoftness` uniform (`shadow.hglsl:7`, radius multiplier `:68`), C++ `gos_SetTerrainShadowSoftness` (`gameos_graphics.cpp:8457`, member `terrain_shadow_softness_`, getter default 2.5f `:8461`; ImGui slider `GraphicsOptionsWindow.cpp:488`, reset 0.9f). **Already a `visual_tuning.json` key** (`shadowSoftness`, `visual_tuning_profile.cpp:160`).
- MC2_SHADOW_CSM gate: swaps single dynamic map → 3-cascade array (`shadow.hglsl:100-238`); signature frozen, call sites unchanged. `mechSoft` uniform gates the extra mech penumbra (`shadow_screen.frag:101`).

### Existing decal / footprint / dust systems
- **`gos_PushDecal(const WorldOverlayVert* verts3, unsigned texHandle)`** = the queue API (`gameos.hpp:2375`, impl `gameos_graphics.cpp:8981`), flushed by `gosRenderer::drawDecals()` (`gameos_graphics.cpp:10319`). State: AlphaBlend SRC_ALPHA/ONE_MINUS_SRC_ALPHA, **depth-test LEQUAL, depth-WRITE OFF, polygon offset (-1,-1)**, world-space MVP. Shader `shaders/decal.frag` — sets `GBuffer1.a=1` (shadowHandled) so decals opt OUT of post-screen shadow (already handle their own shadow inline, :71-77). Runs in the TerrainDecal pass, AFTER terrain overlays, alongside craters/footprints (`gameos_graphics.cpp:2273`, :2246).
- **Footprints ALREADY EXIST and are fully wired** (advisor's "does MC2 have footprints?" = YES): `CraterManager` (`crater.h:26-44` enum `SML/AVG/BIG/…_FOOTPRINT`, `crater.cpp`). Spawned on foot-down animation frames per mech: `mech3d.cpp:3835-3953` `craterManager->addCrater(mechType->{left,right}FootprintType, footPos, rotation)`. Textures `defaults/feet0000.tga` (keyed) + `feet0001.tga` (`crater.cpp:157-166`). Rendered via `gos_PushDecal` world-space (`crater.cpp:573-594`), called from `camera.cpp:2230` + `gamecam.cpp:344`. Gated behind `useNonWeaponEffects` (options.cfg `UseNonWeaponEffects`, `prefs.cpp:255`) and per-mission `footPrints` (`mission.cpp:1081`). Craters also spawned by weapons/vehicles (`weaponbolt.cpp:838`, `mech.cpp:5407` SCORCHMARKS, `gvehicl.cpp:3356`).
- **Dynamic decal ring** — the ideal template for a fading world decal: `mclib/dynamic_decal_ring.h` / `.cpp`, gate `MC2_DYNAMIC_DECALS` (default OFF). Fixed 64-slot ring, NO heap, feeds the EXISTING `gos_PushDecal` path (2 tris/slot), linear alpha fade over final 20% of lifetime, `gatherToDecalBatch(dt)` called from `txmmgr.cpp renderLists()` before `gos_DrawDecals()`. `DynDecal::spawn(worldPos, radius, rotationRad, texHandle, lifetimeMs)`. crater.cpp:264-278 already spawns a ring at each footprint/crater.
- **Dust FX**: particle system `kParticleEffectState` (VFX bridge). Dust clouds already spawned on mech/vehicle move (`gvactor.cpp:1080 !dustCloud`, `missiongui.cpp:4750`). This is the v2 dust lane — NOT needed for grounding v1.

### Cutscene / cinematic camera + lighting hooks
- **Intro/deployment cinematic pan renders through the SAME scene path** as gameplay: `SimpleCamera` block drains `renderLists()` + water + scene-FBO post when `mission!=NULL` (`simplecamera.cpp:219-234` PREVIEW-WORLDLESS-DRAIN-1). So terrain/mech/shadow/decal all appear on the pan with normal renderer state — **a mission-load visual-tuning profile already covers cutscenes** (no separate cutscene render path to hook).
- `MC2_MISSION_INTRO_LEGACY_RENDER` gate (`terrain.cpp:2486`) only toggles solid-arm terrain dependency, not lighting.
- **`visual_tuning.json`** (`GameOS/gameos/visual_tuning_profile.cpp`, docs `docs/visual-tuning-profiles.md`) — flat float key/value, `{defaults, missions:{<name>:{...}}}`, applied at mission load (`visualTuning_applyProfile(missionName)`). Existing keys directly relevant: **`shadowSoftness`**, **`terrainLightingV2Floor`** (black-crush floor, default 0.3, `:166`/`gameos_graphics.cpp:2492`), **`mechAmbientStrength`** (`:208`, gate MC2_MECH_AMBIENT_V1 default-ON), **`exposure`** (`:157`), `terrainLightingV1Strength`, `aoStrength`. Env value-vars win over profile (`envIsSet` guard). Unknown keys warn+ignore (`:254`). Round-trips via "Set as Mission Defaults" (`visualTuning_saveCurrentToMission`).
- **Black-crush landmine (confirmed)**: terrain lighting compute clamps each channel to uint8 255 (`shaders/gos_terrain_lighting.comp:78,84` `if (r>=255) return 255u`); ambient is `eye->ambient{Red,Green,Blue}` uint 0-255 (`gos_terrain_lighting.cpp:845-847`). Daytime ambient saturates HIGH (whiteout), night crushes LOW (black, no readable detail). The remedy for crush is `terrainLightingV2Floor` (raises the multiplicative floor in-shader, NOT the saturating uint8 add) — a per-mission floor lift is the right knob and it is ALREADY a profile key.

### Blob shadow precedent
- **NONE exists.** No projected blob/disc/contact-darkening quad anywhere (searched blob/disc). All unit shadowing goes through the screen-space cascade. No "simple shadows" fallback quad survives on the modern path. A contact disc would be NET-NEW content but can reuse 100% of the decal machinery above.
- No blob/disc texture asset present (`feet*.tga` not even found on disk here — footprints depend on `defaults/feet000{0,1}.tga` being in the install; verify at deploy). A blob would need one small radial-gradient TGA (or the `-0.5` UV "solid white × vertex-color" path in `decal.frag:59` — untextured tint, zero new asset).

---

## 2. OPTION MATRIX (cheapest first)

| # | Option | Mechanism | New assets | Cost | Grounding payoff | Risk |
|---|--------|-----------|-----------|------|------------------|------|
| A | **Contact-darkening blob disc under each unit** | Per-unit `gos_PushDecal` of a radial-alpha disc at foot-center, world-space, existing TerrainDecal pass | 1 small radial TGA OR untextured tint (`decal.frag` `Texcoord.x<-0.5` path) | XS | HIGH — anchors unit to ground, reads even where ground is dark | z-fight (mitigated: polyoffset -1,-1 already), double-darken vs cast shadow |
| B | **Cutscene ambient-lift / floor preset in visual_tuning.json** | Add a `mc2_17`-style mission entry: raise `terrainLightingV2Floor`, `mechAmbientStrength`, soften `shadowSoftness`, tune `exposure` | none (data only) | XS | MED — fixes "lots of black", softens harsh edge | none (data, gated by file presence, env wins) |
| C | Raise mech shadow floor 0.55→~0.65 + widen `mechSoft` for close cam | shader constant / uniform | none | S | MED | affects all tactical shadows unless gated per-cam |
| D | Enable existing footprints/dust in cutscene (`useNonWeaponEffects`, `MC2_DYNAMIC_DECALS`) | flip existing gates | none | S | LOW-MED (already exists) | perf on many units; already shipped machinery |
| E | New per-unit projected soft-shadow quad (real blob w/ light-dir skew) | net-new render pass | asset + code | L | HIGH but redundant w/ cascade | overlaps CSM, double render |

Advisor hint confirmed: **A + B together** beat any expensive terrain feature. E is over-engineering (cascade already casts).

---

## 3. V1 RECOMMENDATION — `GROUND-CONTACT-BLOB-1` (Option A + B)

Two cheap, additive, default-OFF pieces shipped as one slice:

- **(A) Contact blob disc.** New `MC2_GROUND_CONTACT_BLOB` (default OFF). Model directly on `dynamic_decal_ring` — but **persistent-per-frame, not fading**: iterate live movers each frame, `gos_PushDecal` one disc quad (2 tris) per unit at foot-center world pos, radius ≈ unit footprint, argb = a soft dark tint scaled by `eye->ambient` (so it reads against the ground, not pure black). Use the untextured `decal.frag` solid path (`Texcoord.x=-1`) → **zero new asset** for v0, add a radial-gradient TGA later for a soft edge. Gather site: `txmmgr.cpp renderLists()` right before `gos_DrawDecals()` (same site as `gatherToDecalBatch`). Depth-write OFF + polyoffset (-1,-1) inherited from the TerrainDecal pipeline → no z-fight tuning needed.

- **(B) Cutscene grounding preset.** Ship a `data/visual_tuning.json` entry (or documented example) raising `terrainLightingV2Floor` (kill black-crush), `mechAmbientStrength`, and softening `shadowSoftness` for cinematic-heavy missions (mc2_17). Pure data, no gate flip — applied automatically at mission load, covers the cutscene because the pan uses the normal scene path.

Ordering: B first (data-only, proves the ambient-lift half with zero code), then A behind its gate.

---

## 4. GATES
- `MC2_GROUND_CONTACT_BLOB` — new, **default OFF** (byte-identical when unset; follow the `envFlagDefaultOn`-inverse / `MC2_DYNAMIC_DECALS`-style default-off pattern).
- Reuse existing knobs for B: `terrainLightingV2Floor`, `mechAmbientStrength`, `shadowSoftness`, `exposure` (all live profile keys; env value-vars win).
- Interaction: blob must respect `useNonWeaponEffects`? NO — blob is a grounding aid, not an FX; gate it independently on its own flag so it works even with FX off.

---

## 5. TOP LANDMINES
1. **Double-darkening (blob × cast shadow overlap).** Where the cascade already casts the mech's shadow onto the ground AND the blob disc sits, the pixel is multiplied twice → too dark. Cap: blob alpha must be modest (~0.3-0.4 max) AND/OR the blob is `min()`-style rather than additive-multiply. Decal path applies `c.rgb *= shadow` inline (`decal.frag:76`) so the blob quad ALSO gets shadow-multiplied — a blob under a cast shadow is darkened again. Ruling needed: clamp combined floor, or emit blob with its own `shadowHandled` and skip the inline shadow (it already sets GBuffer1.a=1).
2. **Decal z-fight / ordering.** Blob shares the single TerrainDecal batch with craters+footprints; all use depth-test LEQUAL + polyoffset(-1,-1), depth-write OFF. Overlapping decals blend in submit order (no depth resolution between them) → blob under a footprint/crater may show through or over inconsistently. Keep blob radius tight to foot-center; submit blobs FIRST (under) in the batch.
3. **Black-crush is uint8-saturation, not a shader gain.** `gos_terrain_lighting.comp:84` clamps to 255 BEFORE the shader floor; you cannot "add light" to a crushed night scene by raising ambient (it re-saturates the wrong end). Fix crush via `terrainLightingV2Floor` (multiplicative floor lift, post-clamp) NOT by bumping `eye->ambient`. Blob argb should scale with ambient but be clamped so it never goes pure-black on a dark map.

Runners-up: mine/overlay pass sharing the TerrainDecal batch (cement perimeter overlays go through drawTerrainOverlays, a SEPARATE opaque pass — blob is fine, but confirm blob doesn't land in the overlay batch); per-unit decal count perf (N movers × 1 quad = tiny, but N props/vehicles if extended — cap to movers only, Tracy coarse zone `drawDecals`); footprint asset `defaults/feet000{0,1}.tga` may be absent in a bare install (footprints silently no-op) — the untextured blob path sidesteps this.

## 6. ACCEPTANCE
- Static-cam before/after on an mc2_17-like close cutscene scene: mech reads as grounded (contact darkening under feet), ground no longer pure-black-with-no-detail, cast shadow edge softer. RenderDoc/screenshot pair.
- `slice_gate`: `MC2_GROUND_CONTACT_BLOB` unset → byte-identical to baseline (blob gather is a no-op; profile file absent → no-op). Set → blobs appear, no double-darken (verify combined floor ≥ chosen cap).
- No perf regression: Tracy coarse `gosRenderer_drawDecals` zone within noise (movers-only, ≤ N quads); no per-vertex zones added.
- Smoke tier1 5/5 with gate unset (byte-identical) and a spot run with gate set (no crash, no z-fight artifact).

## 7. OPEN RULINGS
1. **Blob shadow-participation**: does the blob get the inline `decal.frag` shadow multiply (double-darken risk) or emit shadowHandled+skip? → Recommend skip (blob provides its OWN darkening; let cascade handle the sharp cast shadow separately) and cap combined darkening.
2. **Blob source**: untextured solid tint (v0, zero asset, hard disc edge) vs radial-gradient TGA (soft edge, needs 1 asset). → v0 untextured, TGA follow-up.
3. **Ambient-coupled alpha**: should blob alpha track `eye->ambient` (dark map → lighter blob so it doesn't crush) or be constant? → couple, with a min/max clamp.
4. **Per-mission preset location**: ship a `data/visual_tuning.json` `mc2_17` entry in-repo vs document-only example the user edits? → ship a documented example entry; env/profile already round-trips via the Options "Set as Mission Defaults" button.
5. **Extend blob to vehicles/props?** → v1 movers only; defer wider objects to v2 (perf + prop-vs-ground contact ruling).
