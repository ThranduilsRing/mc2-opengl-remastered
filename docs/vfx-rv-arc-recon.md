# VFX R→V Lane — Arc Recon (VFX-ARC-RECON-0)

Read-only end-to-end map of the VFX / effects render lane, modeled on the
StaticPropOpaque (completed R→V reference), Terrain (audited), and Mech
(observational) lanes. Goal: surface the authority chain, particle/effect
data model, shader/pass inventory, blend/depth/order matrix, debug/inspector/
capture coverage, invariants, risks, and the next safe slices.

All line numbers grep-confirmed against the `track-rv-VFX` worktree (branched
off `claude/nifty-mendeleev` HEAD `2beef695`) at recon time. Re-grep before
quoting; `weaponbolt.cpp` is ~2600 lines and lines drift.

Sibling recon docs: `docs/mech-rv-arc-recon.md`, `docs/terrain-rv-arc-recon.md`,
`docs/static-prop-rv-arc-audit.md`, `docs/shadow-rv-arc-recon.md` (all
referenced read-only; separate lanes).

Orientation doc `docs/observations/2026-05-25-fx-pipeline-map.md` predates the
B3 ship and is **stale** on the default gate, PpcBolt, CPU-trail suppression,
and the draw primitive — corrections are folded in below.

---

## 0. Scope — what counts as "VFX" in this codebase

VFX is **not one system**. Three architecturally distinct render families
exist, plus two non-particle systems that are explicitly *out of lane*:

| Family | Class | Authority | In VFX lane? |
|---|---|---|---|
| **gosFX particle clouds** (explosions, smoke, muzzle, dust) | GPU billboard *and* legacy CPU MLR leaves | `mclib/gosfx/` (legacy engine) + `mclib/particles/` (GPU bridge) | **YES (primary)** |
| **GPU trails** (missile / PPC bolt trails) | GPU billboard (ring-buffer) | `code/weaponbolt.cpp` feed → `mclib/particles/gpu_trail.cpp` | **YES** |
| **Laser / beam / bolt body** | legacy CPU MLR triangle-list (`gos_vertex`) | `code/weaponbolt.cpp` (INI-driven, CPU-baked vertex color) | **YES (legacy path only)** |
| Craters / footprints / scorchmarks | screen-projected sprite-quads | `mclib/crater.cpp` (`CraterManager`) — **separate system** | NO (classify separately) |
| Terrain "decals" (`decal.frag`) | terrain-overlay fragment program | `terrain_overlay.vert + decal.frag` on terrain | NO (terrain-overlay family) |

**Decision: the VFX lane owns the gosFX/GPU-particle pipeline + GPU trails +
the legacy laser/beam path.** Craters and terrain-decals are non-particle
systems with their own authority and are referenced read-only.

The *only dedicated GPU VFX shader pair* is
`shaders/particle_billboard.{vert,frag}`. There is **no** dedicated GPU shader
for beams/lasers/tracers/muzzle/smoke — those are either legacy CPU `gos_vertex`
geometry (lasers) or producers that feed the one billboard shader (gosFX
clouds, trails).

---

## 1. Authority chain

Two-layer architecture is the central orientation fact:

- **`mclib/gosfx/`** — the *legacy gosFX engine*. Owns effect definitions,
  fcurves, and per-frame lifecycle / emission / age. **Data authority and
  GAMEPLAY-LOAD-BEARING.**
- **`mclib/particles/`** — the *new GPU bridge* (gate `MC2_GPU_PARTICLES`,
  **default ON**). Samples gosFX specs at spawn, fills a 64-byte `GpuParticle`,
  batches, hands to the GL bridge. **Render consumer; safe-to-touch layer.**
- **`GameOS/gameos/gos_particle_bridge.cpp`** — SSBO upload + GL state + draw.

```
mc2.fx (binary effect database on disk)
  └─ EffectLibrary::Load → SpecLibrary::Load        mclib/particles/spec_library.cpp:53
       └─ Effect::Specification::Create per effect   spec_library.cpp:62-67  (m_effectID=i)
            owns ALL spec curves: color/alpha/size/lifeSpan/UV/emission

  [LEGACY per-frame lifecycle — GAMEPLAY-OWNED, do not touch]
  gosFX::Effect (live instance)                      mclib/gosfx/effect.cpp
       ├─ m_age advance                              effect.cpp:625, 741   (age in [0,1])
       ├─ m_lifeSpan sampled at start                effect.cpp:567-570
       └─ subclass Draw() → [gated] Spawn to GPU     card.cpp:505-506, cardcloud.cpp:503-504,
                                                     pointcloud.cpp:478-479, shardcloud.cpp:322-323,
                                                     tube.cpp:1175-1176

  [NEW GPU bridge — render-only, gated MC2_GPU_PARTICLES (default OFF)]
  mc2::particles::Spawn(spec, parentToWorld, seed)   mclib/particles/spawn.cpp:24
       └─ switch on spec->GetClassID()               spawn.cpp:33-55
            └─ SpawnCard / SpawnCardCloud / SpawnPoint / SpawnShard / SpawnTube
                 spawn_card.cpp:43   (canonical producer)
                 ├─ sample spec curves at age=0.5     spawn_card.cpp:71, 80-122
                 ├─ Batcher::BeginGroup(handle,UV,blend) spawn_card.cpp:150
                 └─ Batcher::Emit(GpuParticle)           spawn_card.cpp:172

  Batcher (singleton, per-frame)                     mclib/particles/batcher.{h,cpp}
       ├─ Emit → staging vector                      batcher.h:90
       ├─ ResolveTextures (MLR idx→gos handle)       batcher.h:95
       └─ Flush → bridge; CLEARS staging every frame batcher.h:102  (no persistence)

  gos_particle_bridge_flush                          gos_particle_bridge.cpp:190
       ├─ program particle_billboard                 :108
       ├─ SSBO upload (binding=14)                    :53,156
       ├─ per-group glDrawArrays(GL_TRIANGLES, n*6)   :325,398
       └─ blend from GroupInfo.blendMode             :388-391
```

**Ownership table:**

| Datum | Owner / authority | Site |
|---|---|---|
| effect parameter database | `mc2.fx` binary → `SpecLibrary::m_effects` | load `spec_library.cpp:53` |
| per-effect spec curves (color/size/UV/lifetime) | `gosFX::Effect::Specification` subclass | `mclib/gosfx/card.cpp` etc. |
| live effect age/lifecycle/emission | `gosFX::Effect` (legacy) — **gameplay** | `effect.cpp:625,741`; `particlecloud.cpp:399,458` |
| per-particle color/alpha (GPU) | sampled from spec curves **at age=0.5** | `spawn_card.cpp:89-92` |
| per-particle size | spec `m_halfHeight`×`m_aspectRatio` | `spawn_card.cpp:80-87` |
| per-particle lifetime | spec `m_lifeSpan.ComputeValue` | `spawn_card.cpp:96-97` |
| per-particle world position | `parentToWorld` translation | `spawn_card.cpp:104-107` |
| texture handle | spec `m_state.GetTextureHandle()` (MLR pool idx) | `spawn_card.cpp:112` |
| UV sub-rect / atlas cell | spec `m_UOffset/m_VOffset/m_USize/m_VSize` | `spawn_card.cpp:119-134` |
| blend mode (alpha vs additive) | spec `m_state.GetAlphaMode()` → 0/1 | `spawn_card.cpp:139-145` |
| GL staging / SSBO / draw | `Batcher` + `gos_particle_bridge` | `batcher.cpp`, `gos_particle_bridge.cpp` |
| laser/bolt body color | weapon-type `.ini` (`FrontRGB`...`BackRGB`), BGR→RGB, CPU-baked into vertex `argb` | `weaponbolt.cpp:161-243`, `:309-314`, `:1689-2001` |
| GPU-trail color | hardcoded tuning table (MissileSmoke white, PpcBolt blue) | `gpu_trail.cpp:51-63,155-180` |

**GpuParticle struct (64B std430, `mclib/particles/spec.h:42-52`; GLSL mirror
`shaders/include/particles.hglsl` — change in lockstep):**

```cpp
struct GpuParticle {
    float    position[3];   // 00  world-space spawn origin
    float    _pad0;         // 12
    float    color[4];      // 16  straight RGBA (not premultiplied)
    float    velocity[3];   // 32  world per-second delta
    uint32_t kind_flags;    // 44  [0]=is_head, [7:4]=kind
    float    lifetime;      // 48  total seconds
    float    age;           // 52  current age seconds (set 0; NOT advanced GPU-side)
    float    size;          // 56  world radius (meters)
    uint32_t atlasIndex;    // 60  MLR pool idx → resolved to GLuint at flush
};  // static_assert sizeof==64, spec.h:62-75
```

**No persistence.** `Batcher::Flush()` clears staging + groups every frame
(`batcher.cpp:251-252`). Trails self-buffer: `WeaponBolt` keeps a
`trail_history[24]` ring on the bolt and re-stamps each frame
(`weaponbolt.cpp:1610-1638`).

---

## 2. The age=0.5 snapshot — central render-quality fact

The GPU path samples **all curves at a fixed `age = 0.5f`**
(`spawn_card.cpp:71`, documented `:63-70`) because the billboard shader does
**not advance age or evaluate curves** — `particle_billboard.frag:30` is just
`outColor = tex * v_color`. Sampling at age=0 baked invisible/degenerate
particles (alpha rising from 0, halfHeight growing from 0); age=0.5 picks peak
visibility. Consequence: **particles do not animate** (no fade-in/out, no
growth/shrink). This is the single biggest render-quality lever and is
render-only — a future visual slice could move age advance + curve eval
GPU-side without touching any gameplay site (§7). See
[[gpu-particle-age-zero-curve-trap]] in INDEX-RENDERING.

---

## 3. Shader / pass inventory

| Shader | Role | View transport / notes |
|---|---|---|
| `shaders/particle_billboard.vert` | GPU billboard vertex; reads `Particle` SSBO (binding=14); `gl_VertexID`-driven quad expansion (6 verts/particle); no vertex attributes | **LEGACY flat `uniform mat4 u_worldToClipGL`** (`:30`); does NOT include `view_uniforms.hglsl`; uploaded from `gos_GetTerrainMVPMat4()` at `gos_particle_bridge.cpp:279-281` |
| `shaders/particle_billboard.frag` | samples `uAtlas` via `textureLod(...,0.0)` (AMD auto-LOD fix); color-key magenta `0xFF00FF`→discard; `outColor = tex*v_color`; head sprites ×1.5; discard a<0.01; **single output, no GBuffer, no objectId** | no debug branch, no fog/light/shadow |
| `shaders/include/particles.hglsl` | `struct Particle` std430 mirror of `GpuParticle` | lockstep with `spec.h` |
| (legacy) laser/beam | **no GLSL shader** — CPU `gos_vertex` MLR triangle-list | `weaponbolt.cpp:541-554` `addTriangle(... MC2_ISEFFECTS)` → `renderLists()` |
| `shaders/decal.frag` + `terrain_overlay.vert` | craters/terrain overlay — **separate system, read-only** | legacy flat `u_worldToClipGL`; writes GBuffer1, fog, shadow |

**No mech-style `u_debugMode` hook exists in any VFX shader** (grep: 0 hits).
Unlike the mech lane (which had 9 pre-existing modes to surface), a VFX
debug-view slice must **add** a new uniform + branch from scratch.

**Pass contract:** `RenderCore/RenderPassContract.h:139-149` — `RenderPassId::VFX=5`,
name "VFX", owner `mc2::particles::Batcher`, `viewUniformsBound=false`,
`pipelineDescRegistered=false`, `snapshotRowAuthoritative=false`,
inspectorSectionId `"VFX Pass##vfx"`, **`killSwitchEnv=nullptr`** (under-reports;
the real master gate is `MC2_GPU_PARTICLES`). Note "Object-ID PROHIBITED.
GpuTrailKind {None, MissileSmoke, PpcBolt}."

---

## 4. Blend / depth / order matrix

| Path | Blend | Depth-test | Depth-write | Cull | Evidence |
|---|---|---|---|---|---|
| GPU particles (alpha default) | `SRC_ALPHA, ONE_MINUS_SRC_ALPHA` | ON, `GL_GEQUAL` (reverse-Z) | **OFF** | OFF (double-sided) | blend `gos_particle_bridge.cpp:306-307`; depth `:303-305`; cull `:269` |
| GPU particles (additive group) | `SRC_ALPHA, GL_ONE` | (inherits) | OFF | OFF | `gos_particle_bridge.cpp:388-392` (`grp.blendMode==1`) |
| GPU trails | (inherits group blend) | ON, GEQUAL | OFF | OFF | same bridge |
| Laser/beam (legacy MLR) | MLR alpha (`MC2_ISEFFECTS`/`MC2_DRAWALPHA` bit-flags, `txmmgr.h:54-57`) | via `renderLists()` | per MLR | — | `weaponbolt.cpp:541-554` |
| Crater/footprint (separate) | `SRC_ALPHA, ONE_MINUS_SRC_ALPHA` | ON GEQUAL | OFF | OFF | `gameos_graphics.cpp:7993-7998` |

The bridge fully saves/restores prior GL state (`gos_particle_bridge.cpp:253-269`
save, `:405-415` restore). Reverse-Z compare (`GL_GEQUAL`) is **correct**
(`:304`). `MC2_DRAWALPHA/MC2_ISEFFECTS/MC2_DRAWONEIN` are addTriangle bit-flags,
NOT env vars.

**objectId / GBuffer / fog / light / shadow:**

| Path | objectId | GBuffer1 | fog | lighting | shadows |
|---|---|---|---|---|---|
| GPU particles | **NO** (enforced) | NO | NO | NO | NO |
| crater/decal (separate) | NO | YES | YES | partial | YES |

GPU particles write **none** of these. The objectId prohibition is mechanically
enforced by `scripts/check-vfx-no-objectid.sh` (integer attachment-2 doesn't
blend; a blended particle would clobber underlying mech IDs and break
mech-pick).

**Frame-loop ordering (`code/gamecam.cpp`):**

```
sky/HDRI                         ~:267-282
terrain (land->render)           ~:284-288
craters/footprints               ~:290-294   ← separate sprite-quads
objects/mechs                    ~:296-303
water (renderWater)              ~:305-308
shadows (renderShadows)          ~:310-314
VTOL/compass                     ~:316-329
mcTextureManager->renderLists()  ~:333-334   ← MLR flush: lasers, CPU gosFX geom, beams
waterFastPath                    ~:343-346
── GPU PARTICLE FLUSH ──         ~:356-378
   gos_SetActiveCamera (axis-swapped right/up)  :371
   Batcher::ResolveTextures()                    :374
   Batcher::Flush() → bridge SSBO + glDrawArrays :375
theClipper->RenderNow() "Draw the FX" (CPU gosFX leaves) ~:394
weather                          ~:398-402
DebugRenderer prims              ~:404-409
[postprocess: bloom/fxaa/tonemap + god rays in gos_postprocess]  later
```

GPU particles composite **after** terrain/mechs/water/shadows and after
`renderLists()` (so scene depth is populated, trap #6 `:348-352`), but **before**
the CPU MLR clipper's `RenderNow`, weather, and post-process — i.e. forward-
composited into the lit scene color buffer, **pre-bloom/pre-tonemap**. Bloom
therefore *does* pick up bright/additive particles (relevant to a future
additive-clamp slice).

---

## 5. Current debug / inspector / resource / capture coverage

VFX is **structurally further along than mech was at recon** — it already has
a RenderPassContract row AND a read-only ImGui inspector panel ("VFX-SPINE-0"),
but is closure-red on every Track-V substrate axis below.

### Gap table vs reference lanes

| Axis | StaticPropOpaque | Mech | **VFX** |
|---|---|---|---|
| RenderDebugView mask | 7 modes | 4 modes | **`kDebugViewMask_Vfx = 0u`** (`RenderDebugView.h:45`) |
| debug-view shader consumer | static_prop.frag | mech.frag `u_debugMode` | **NONE** (no `u_debugMode` in particle shaders) |
| inspector debug-view combo | yes | yes | **none** (mask=0 → empty) |
| EngineView | MainScene | MainScene | **none** (particle flush uses `gos_SetActiveCamera`, separate basis) |
| RenderResourceRegistry | MaterialGpuBuffer | (shares) | **none** (particle SSBO b14, trail ring, fx atlas all unregistered) |
| RenderPassContract row | id=1 (3 axes true) | id=3 (snapshot true) | **id=5 present**, all 3 axes false, killSwitch=nullptr |
| PipelineId | 1,2 | none | **none** (`Count_=3`) |
| debug-state JSON section | `staticPropOpaque{}` | `mech{}` | **none** |
| ImGui inspector panel | StaticProp | Mech | **YES** "VFX Pass##vfx" (`EditorInspector.cpp:1035-1095`), read-only |
| env kill-switch | `MC2_SNAPSHOT_STATIC_PROP_BUILD` | `MC2_SNAPSHOT_MECH_EXTRACT` | `MC2_GPU_PARTICLES` (default OFF; =1 enables) |
| visual baseline / capture preset | staticprop_baseline_01/02 | mech_24, mech_17 | **none** |

### Existing assets to reuse

- **ImGui panel** `GuiRuntime/EditorInspector.cpp:1035-1095` ("VFX-SPINE-0"),
  driven by `s_vfxPass` (`:89`), filled via `setVfxPassSnapshot()` (`:171-173`)
  once/frame from `gameosmain.cpp:1502`. Shows: enable/log state, a
  ViewUniforms-not-consumed warning, `particle_billboard` program id, SSBO
  capacity, process-lifetime emit/flush/trail counters, per-kind draw counts as
  **"n/a"** (no per-kind counter exists). Read-only; no mutating controls.
  Struct `VfxPassSnapshot` at `EditorInspector.h:121-146`.
- **debug-state JSON template:** mirror the gated `mech{}` block
  (`debug_state_dump.cpp:242-299`) — keys off `featureActive(...)`, emits zeroed
  counters + `"packets": []` when off. A `vfx{}` block would reuse the
  in-process `VfxPassSnapshot`.

### Env vars (VFX)

| Env | Default | Meaning | Site |
|---|---|---|---|
| `MC2_GPU_PARTICLES` | **ON** (B3c-2) | master gate; `Batcher::is_enabled()` is single source of truth | `batcher.cpp:82` |
| `MC2_GPU_PARTICLES_LOG` | OFF | SPAWN/RESOLVE/TRAIL_PROBE logging | `batcher.cpp:80` |
| `MC2_GPU_TRAIL_DISABLE` | OFF (trails on) | force-off trail emitter only | `gpu_trail.cpp:80` |
| `MC2_GOSFX_GROUP_LOG` | OFF | bridge UV/missing-texture/camera logging | `gos_particle_bridge.cpp:222` |
| `MC2_FX_TRACE` | OFF | stderr FX-event histogram at mission end (smoke neutral counter) | `mclib/fx_trace/fx_trace.cpp:93` |

**None of these are in `docs/tier1_env_vars.md`** (gap — fix when a VFX env is
added in slice 2).

### Default-ON / mismatch findings

- **`MC2_GPU_PARTICLES` default is OFF** (reader: `GameOS/gameos/gos_particle_bridge.cpp`; the earlier `batcher.cpp:82` citation needs re-verification). `effect.cpp:43` is a *diagnostic
  counter* gate (`==1` only), NOT a render gate — it does not suppress
  producers.
- **UI/runtime mismatch:** `GuiRuntime/GraphicsOptionsWindow.cpp:99` lists
  `MC2_GPU_PARTICLES` with options-menu default **`false`**, contradicting the
  runtime default ON. The menu checkbox shows unchecked while particles render.
  Cosmetic (the runtime gate wins) but worth a one-line fix.
- Inspector header comment `EditorInspector.h:125` correctly says "default ON".

---

## 6. Known invariants

- **VFX writes NO object-ID** (`check-vfx-no-objectid.sh`). Integer
  attachment-2 doesn't blend; a blended particle would clobber mech IDs and
  break picking. Any debug-view slice must preserve this.
- **GpuParticle is a 64B std430 ABI** pinned by `static_assert`
  (`spec.h:62-75`) with a GLSL mirror (`particles.hglsl`). Any field change is a
  lockstep C++/GLSL edit + reflect golden drift.
- **Batcher has zero persistence** — staging cleared every Flush. Producers own
  trail history.
- **age=0.5 snapshot** is load-bearing for visibility (§2); do not revert to
  age=0 without GPU-side curve eval.
- **Stuff→MC2/GL axis swap** is load-bearing for the particle camera basis
  (`gos_SetActiveCamera` at flush) AND for producer positions
  (`weaponbolt.cpp:1618-1620` `(-x,z,y)`). See [[gpu-particle-age-zero-curve-trap]]
  cluster.
- **Reverse-Z** (`GL_GEQUAL`); particle depth compare is correct.

---

## 7. Risk list

- **R1 — Lifetime/emission/age are GAMEPLAY-LOAD-BEARING.** `gosFX::Effect`
  age advance (`effect.cpp:625,741`), `m_particlesPerSecond`/`m_maxParticleCount`
  (`particlecloud.cpp:399,458`), `lifeSpan` authoring, and projectile flight
  (`weaponbolt.cpp:1156`) must NOT be modified by any render slice. The
  render-safe layer is `mclib/particles/*`, `gos_particle_bridge.cpp`, and
  `particle_billboard.{vert,frag}` only. (HIGH.)
- **R2 — Object-ID firewall.** Adding any GBuffer/objectId write to the
  particle shader breaks mech-pick and trips `check-vfx-no-objectid.sh`. A
  debug-view slice must keep the single `outColor` output. (HIGH.)
- **R3 — No pre-existing debug hook.** Unlike mech.frag, particle shaders have
  no `u_debugMode`. A VFX-DEBUG-VIEWS slice is a *new* uniform + branch + a new
  upload path (`gos_particle_bridge`), so it is a real shader_reflect golden
  drift, not a no-op surface. Mode 0 (Final) must be byte-identical. (MED.)
- **R4 — Overdraw / blend cost.** Particles are depth-write-OFF, double-sided,
  alpha/additive, composited pre-bloom. Heavy explosion/smoke scenes are an
  overdraw risk; bloom amplifies additive overdraw. This is exactly what
  VFX-OVERDRAW-AUDIT-0 must measure before any visual change. (MED.)
- **R5 — Bloom/tonemap interaction.** Because particles composite BEFORE
  bloom/tonemap (§4), an additive-brightness or clamp slice interacts with the
  postprocess lane — but postprocess edits are OUT of Batch 1 unless recon
  proves dependence. (MED, future.)
- **R6 — Legacy view transport.** Particle vert is on flat `u_worldToClipGL`,
  not ViewUniforms (b3). A future VFX-VIEWUNIFORMS slice is a real shader+reflect
  change, not a no-op. Defer. (MED, future.)
- **R7 — age=0.5 is a workaround, not a feature.** A naive "animate particles"
  visual slice must add GPU-side age advance + curve eval; doing it wrong
  re-introduces the invisible-at-age-0 trap. (MED.)
- **R8 — Dual-draw uncertainty.** gosFX clouds still run legacy `::Draw` (MLR)
  AND `Spawn` to the batcher when enabled (`card.cpp:507`). Comments claim the
  MLR work-leaves no-op (`effect.cpp:804-805`) but this was not traced — possible
  double-render for explosions/smoke. Confirm in VFX-OVERDRAW-AUDIT-0. (MED.)
- **R9 — Unrouted gosFX classes.** Only 5 ClassIDs route to GPU (Card,
  CardCloud, Point, Shard, Tube; `spawn.cpp:33-55`). Shape/Pert/Spinning/Debris/
  PointLight have NO Spawn route and stay CPU-only. Muzzle-flash GPU coverage
  unverified (its spec ClassID not checked). A debug view will only see the 5
  routed classes. (LOW, honesty of coverage.)
- **R10 — Atlas UV debt.** GPU trail hardcodes full-page `(0,0,1,1)` UV, so
  atlased smoke renders as white squares (orientation K3). A texture/atlas
  cleanup is a candidate visual slice but NOT a cook rewrite. (LOW.)
- **R11 — Trail texture wiring.** Some gosFX groups arrive `handle=0` (not
  texture-wired, `gos_particle_bridge.cpp:347-358`). Inventory/debug must
  tolerate unresolved handles. (LOW.)

---

## 8. Recommended next slices

Ordered lowest-risk-first, each gated default-OFF, no default visual change,
mirroring the StaticProp/Terrain/Mech lane pattern.

1. **VFX-DEBUG-VIEWS-1 (Batch 1, slice 2).** Add a particle-shader
   `u_debugMode` uniform + branch + upload path, wire `kDebugViewMask_Vfx` to
   the truthful subset and the inspector combo. **Only data-backed modes:**
   Final(0, byte-identical), Albedo/texture(1, `tex` only), Alpha(2,
   `v_color.a` ramp), Blend-mode(5, additive-vs-alpha from `kind_flags`/group),
   ParticleKind(6, `kind_flags[7:4]`), Overdraw-proxy(7, constant-add for
   blend accumulation — cheap). **Omit** Lifetime/age (data present but not
   GPU-advanced — would mislabel; defer to the age-eval visual slice),
   Depth/soft-particle (no soft-particle data yet), Fog (no fog in shader).
   New env `MC2_VFX_DEBUG_MODE`; add to `docs/tier1_env_vars.md` + env_registry;
   shader_reflect hygiene; confirm `check-vfx-no-objectid.sh` still passes.
   Mode 0 byte-identical. Validate: build RelWithDebInfo, tier1 5/5,
   VFX-heavy mission (mc2_10/mc2_24 combat) with mode enabled.

2. **VFX-BASELINE-0 (Batch 1, slice 3).** First VFX capture presets +
   metadata. VFX-heavy missions: combat with weapon fire (mc2_10, mc2_24).
   Capture default + the slice-2 debug modes. Metadata: `MC2_VFX_DEBUG_MODE`,
   `MC2_GPU_PARTICLES`, mission, preset, commit, resolution. Transient-effect
   timing: add a start-delay / scripted-fire note rather than changing
   gameplay. First VFX entries in `tests/visual/baselines/` + `presets.json`.

3. **VFX-OVERDRAW-AUDIT-0 (Batch 2, slice 4).** Measure overdraw/blend cost
   before any visual change. Extend the existing `VfxPassSnapshot` /
   inspector with per-frame particle count, per-kind counts, draw-group count,
   texture binds; add a blended-pixel proxy if cheap. Resolve R8 (dual-draw)
   and R9 (unrouted classes). Deliverable `docs/vfx-overdraw-audit.md`.

4. **VFX-TUNING-UI-1 (Batch 2, slice 5).** Expose safe tuning (global VFX
   intensity, additive brightness scale, smoke alpha scale) as
   gated/reset-to-default controls — only if real values back them. No default
   change, no lifetime/emission change.

5. **VFX-VISUAL-PLAN-0 (Batch 2, slice 6, doc only).** Plan first visual.
   Open questions already answered: particles don't animate (age=0.5, §2);
   composite pre-bloom (§4); no soft-particle data; legacy view transport;
   atlas UV debt (R10). Likely first visual = GPU-side age advance + curve eval
   (true fade/grow) OR alpha/additive tuning — NOT a texture/cook rewrite.

**Deferred / not authorized:** VFX-SOFT-PARTICLES-1 (needs scene-depth sample),
VFX-VIEWUNIFORMS-1 (R6), VFX-ADDITIVE-CLAMP-1 (R5, postprocess-adjacent),
VFX-FOG-COHERENCE-1 (no fog in shader), any cook/atlas rewrite.

**Verdict:** VFX lane is at R-stage with a partial spine (pass contract +
read-only inspector already exist) but zero Track-V substrate (no debug views,
no JSON section, no baselines, no resource registration). Slices 2–3 bring it
to StaticProp/Terrain/Mech debug-parity with zero default-visual risk. A first
*visual* slice is NOT yet justified — it needs VFX-OVERDRAW-AUDIT-0 (to bound
the dual-draw/overdraw risk) and VFX-VISUAL-PLAN-0 first.
