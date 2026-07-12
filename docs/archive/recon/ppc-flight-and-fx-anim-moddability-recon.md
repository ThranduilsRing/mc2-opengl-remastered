# PPC Flight Jank Fix + FX/Animation Moddability — Recon

> **STATUS (2026-06-15): SHIPPED.** PPC speed mult (`MC2_PROJECTILE_SPEED_MULT`, `0a3a852c`)
> + de-curve straight-flight w/ spawn-velocity lead (`MC2_DIRECT_FIRE_STRAIGHT`, `4435d835`),
> both env-gated default-off + launcher-toggleable. FX moddability: `anims.json` anim-override
> registry (`96c50ad0`) + `tools/mc2weapon` weapon editor. Separately, the GPU gosFX FX path
> was REVERTED to default-OFF (CPU/MLR) — `4ad31a9e` — because the GPU oracle looked broken;
> the Q1 energy-beam visual upgrade was NOT done (it stays an option only if the GPU path is
> re-enabled and fixed). See memory `fx_gpu_oracle_reverted_default_cpu`.

Scouting report. Branch `claude/fx-tracy-cost-split` (off advanced nifty). Two asks:
(1) fix the janky "hit/miss-rolled-at-spawn then curve-to-target" projectile flight —
speed PPC 2-3×, disable curving for direct-fire, KEEP arc for LRMs; (2) deep
moddability for swapping FX **and** animations. Caveman-compressed; all file:line
preserved. Re-grep before editing (lines drift).

---

## PART 1 — PPC / projectile flight jank

### The mechanic today (the jank), end-to-end
1. **Fire + hit/miss roll AT SPAWN** — `BattleMech::fireWeapon` `code/mech.cpp:7994-8377`.
   - `attackChance = calcAttackChance(...)` (8122); `hitRoll = RandomNumber(100)` (8124).
   - HIT vs MISS decided at 8142 `if (target && hitRoll < attackChance)`. Result stored as
     `hitLocation` in `WeaponShotInfo shotInfo` (8300-8305): `-1` = miss, `0-7` = body loc.
   - Bolt spawned `createWeaponBolt(effectType)` (8263/8346), then `connect()`:
     HIT → `weaponFX->connect(this, target, &shotInfo,...)` (8280/8367) [tracks target];
     MISS → `weaponFX->connect(this, *targetPoint, &shotInfo,...)` (8282/8373) [aim point].
2. **Per-frame curve-to-target** — `WeaponBolt::update()` `code/weaponbolt.cpp:381-2050`.
   - **Re-tracks the CURRENT target pos EVERY frame** (the homing/jank): `setTargetPosition(...)`
     (559-589) then `laserVelocity.Subtract(targetPos, ownerPosition)` (588). So even direct-fire
     bolts steer toward the moving target each frame.
   - **Arc (LRM only)** gated by `arcEffect`/`arcHeight` (594-607): vertical `goalHeight` steering.
   - **Move:** normalize `laserVelocity`, `*= velMag` (1172), `laserPosition += laserVelocity` (1177).
   - **Hit detection by proximity** (613-622): when `lastDistanceMoved >= distance` (bolt passed
     target) → `hitTarget=TRUE` → `target->handleWeaponHit(&weaponShot)` (753-824) applies the
     PRE-ROLLED damage/location. **Damage outcome is pre-decided; flight is cosmetic-ish but
     gates WHEN damage lands.**
3. **Speed** — `WeaponBoltType::velocity` (`code/weaponbolt.h:62`), loaded `Velocity=<f>` from the
   bolt .fit/INI (`weaponbolt.cpp:213` `readIdFloat("Velocity",velocity)`), applied `velMag` (586) → 1172.

### Weapon-type discrimination (the curve/no-curve predicate)
- Component form enum `mclib/cmponent.h:52-74`: `COMPONENT_FORM_WEAPON_ENERGY`,
  `_BALLISTIC`, `_MISSILE`. Ammo enum `:76-83`: `WEAPON_AMMO_LRM/SRM/...`.
- `fireWeapon` already branches on `getForm()==COMPONENT_FORM_WEAPON_MISSILE` (mech.cpp:8088).
- **Predicate "should curve/track":** TRUE iff missile (`COMPONENT_FORM_WEAPON_MISSILE`, or
  ammo LRM/SRM). FALSE for ENERGY (PPC/laser) + BALLISTIC (autocannon/gauss).
- The bolt already exposes `arcEffect`/`isBeam` on `WeaponBoltType`; the cleanest carrier is a
  per-bolt-type `bool` (data-driven from the bolt .fit) OR derive from the firing weapon's form.

### Damage-safety ruling (CRITICAL — confirmed by scout)
Disabling per-frame re-tracking does NOT change hit/miss outcome: `hitLocation` is pre-rolled and
`handleWeaponHit` fires on proximity regardless of path. A straight bolt to the **pre-rolled aim
point** still triggers the proximity hit. **One subtlety:** if a "hit" bolt aims at the target's
spawn-frame position and the target MOVES during flight, a straight (non-tracking) bolt could pass
behind it and the proximity check might not trip → visual miss on a rolled hit. **Fix: at spawn,
for direct-fire HIT bolts, set the aim point to a lead/intercept point (or just the target's
current hit-node), and freeze it** — don't re-track. With PPC sped 2-3× the flight time shrinks,
so lead error shrinks too (the speed-up directly de-risks the de-curve).

### Recommended fix (3 small changes, data-gated, reversible)
**(a) PPC speed 2-3×** — cleanest = DATA: edit `Velocity=` in the PPC bolt .fit (no recompile).
  Find via the PPC `compbas.csv` "Special FX ID" → bolt def. Fallback CODE path: scale `velMag`
  by a per-bolt-type multiplier. Prefer data. Apply same to ER-PPC.
**(b) Disable curving for direct-fire** — in `WeaponBolt::update()` (~559-589), gate the
  per-frame `setTargetPosition` re-track behind `shouldTrack` (= bolt is missile/arc). Direct-fire:
  freeze `targetPosition` at the spawn value. Leave the arc block (594-607) untouched (already
  `arcEffect`-gated → LRM only). One branch, behaviorally additive.
**(c) Spawn-time aim for direct-fire HIT** — in `fireWeapon` HIT path (mech.cpp:8280/8367) for
  non-missile forms, connect to a frozen lead point (current hit-node pos) rather than a live
  target handle, so the de-curved bolt still lands.

**Gating:** put the de-curve + speed behind an env flag (e.g. `MC2_DIRECT_FIRE_STRAIGHT=1`,
default-off) for A/B, plus the data velocity bump. Gate via the **MC2_FX_FORCE_SPAWN fixture**
(it fires PPC/laser/AC/gauss on mc2_01, +gauss on mc2_24) — capture before/after visually.
**Effort M, risk MED** (gameplay/damage timing). NOT FX-system code — this is `weaponbolt.cpp` +
`mech.cpp` projectile sim. Needs interactive visual confirm (headless can't see the trajectory).

**Risks:** AI lead-targeting (`calcAttackChance` already accounts for travel time — faster bolt may
shift hit% slightly; verify), FX trail length tuned to old slow speed (trail may look stubby at 3×
→ may want to lengthen the tube/trail lifetime), very-close-range proximity check granularity at
high speed (bolt step per frame grows; ensure it doesn't skip past a near target — clamp step or
sub-step). All surfaceable via the fixture.

---

## PART 2 — FX + Animation moddability

Project already has more infra than expected (other sessions shipped it). Prior docs:
`docs/gosfx-animation-lifecycle-recon.md`, `docs/gosfx-modder-dropin-path.md`. This synthesizes
+ adds the actionable gaps.

### FX (gosFX) — chokepoint + existing tooling
- **Load chokepoint:** `SpecLibrary::Load(MemoryStream*)` `mclib/particles/spec_library.cpp:53`
  (5 call sites open `data/effects/mc2.fx`). Field inventory unchanged (curves: Constant/Linear/
  Spline/Complex + `SeededCurveOf`, parsed `fcurve.cpp:102`). Modder-relevant: lifetime
  (`m_lifeSpan`), color/alpha (`m_pRed/Green/Blue/Alpha` particlecloud.hpp:50-81), scale, texture-
  frame anim (`m_index`,`m_animated`,`m_U/VOffset/Size` card.hpp:62-75), physics
  (`m_startingSpeed`,`m_pAcceleration*`,`m_pDrag`), emission (`m_particlesPerSecond`), blend+texture
  (`MLRState`).
- **Tooling ALREADY SHIPPED:** `tools/mc2fx/` CLI (`dump`/`build`/`clone`/`new`) emits a patched
  `mc2.fx` via stream-splice + the `EffectLibrary::Save` path (`effectlibrary.cpp:81`). `patch.json`
  = `{"edits":[{"effect":NAME,"set":{FIELD:{"type":"constant","value":N}}}]}`.
- **Overlay deploy WORKS TODAY:** drop `mods/<id>/data/effects/mc2.fx`, `MC2_ACTIVE_MOD=<id>` →
  `File::open`/`TryModOpen` (`file.cpp`, base<dep<active first-wins) resolves it.
- **Weapon→FX binding (pure data):** `compbas.csv` "Special FX ID" → `effects.csv`
  (effectName/hitEffectName/missEffectName) → `EffectLibrary::Find(name)`. Or `<weapon>.fit`
  `HitEffect=`/`MissEffect=`.
- **LIMITS:** whole-blob replace only (`SpecLibrary::m_effects` frozen `DynamicArrayOf`,
  `m_effectID=index` → no runtime per-effect append); JSON edits = constant-curve only (no
  keyframe lists / seed subcurves / MLRState texture+blend / emitter physics authoring yet).
  **gosFX particles default-DISABLED in beta** (`MC2_DISABLE_GOSFX=1`, `mlr/mlr_gate.h`).

### Animations — model + chokepoint (NOTE: NOT skinned skeleton)
- MC2 uses **TGL** mesh lib (`mclib/msl.*`, `tgl.*`), class `TG_AnimateShape`. Animation =
  **per-NODE TRS keyframes** (`_TG_Animation`: `nodeId` + `UnitQuaternion quat[frames]` +
  `Point3D pos[frames]` + frameRate, `msl.cpp:2458-2516`). NOT skinned, NOT per-vertex.
- **Cook chokepoint (the SpecLibrary::Load analog):**
  `TG_AnimateShape::LoadTGMultiShapeAnimationFromASE` `mclib/msl.cpp:2608`. Source = 3DS-MAX
  `.ase`; cooked to binary `.agl` (`{tglPath}{name}.agl`), mtime+version-gated
  (`CURRENT_ANIM_VERSION 0xBADDECAF`, msl.cpp:76). `.agl` is a simple tool-emittable layout
  (msl.cpp:2519-2575). Access via `gos_FileExists`/`gos_OpenFile` → **routes through mod overlay.**
- **Gesture→clip binding (convention, in mech .ini):** `Mech3DAppearanceType::init`
  `mclib/mech3d.cpp:309`. `[Gestures0..24]` blocks; clips loaded by filename convention
  (mech3d.cpp:550-578): `<mech><MechAnimationNames[i]>.ase` where `MechAnimationNames[]`
  (mech3d.cpp:265-294) maps gesture→suffix (`Walk`/`Run`/`Idle`/`Jump`/`HitFront`/...). Gesture IDs
  `mech3d.h:64-88`. Missing file = NULL (graceful freeze).
- **GLTF import does NOT cover animation:** `[Import] Source=<glb>` (mech3d.cpp:367-449) →
  `ImportGeometryFromFile` (`assimp_importer.cpp:483`) reads meshes only; logs but ignores
  `mNumAnimations` (line 506). GLB = static geometry; anims stay legacy `.ase`/`.agl`. GLB also
  bypasses `File::open` (raw assimp) → overlay routed per-record via the registry below.
- **Existing overlay template:** `mclib/model_override_registry.cpp` — JSON additive overlay,
  base + `mods/<MC2_ACTIVE_MOD>/data/model_overrides/models.json` merge, mod-wins. Record
  `{"type":"model","replaces":"<class>:<name>","source":"x.glb","renderOnly":true,...}`. Classes
  today: `staticProp`/`tree` only. **No anim/mech override class yet.** Safety: rejects abs/`..`/non-glTF.

### Animation swap WORKS TODAY (zero engine change)
Drop `mods/<id>/data/tgl/<mech>Walk.ase` (or pre-cooked `.agl`), set `MC2_ACTIVE_MOD` → overlay
resolves, engine recooks. The naming convention IS the implicit manifest. **Smallest valuable slice
= document + tool this; needs NO code.**

### Unified moddability design + gaps
| Subsystem | Chokepoint | Asset | Status today |
|---|---|---|---|
| FX | `SpecLibrary::Load` (spec_library.cpp:53) | `data/effects/mc2.fx` blob | whole-blob overlay ✅ + `mc2fx` CLI ✅ |
| Anim | `LoadTGMultiShapeAnimationFromASE` (msl.cpp:2608) | `<mech><suffix>.ase`→`.agl` | per-file drop-in ✅ (recooks) |

**Proposed additive JSON registry** (mirror `model_override_registry.cpp`), e.g.
`mods/<id>/data/anim_overrides/anims.json`:
```json
{"overrides":[
  {"type":"anim","replaces":"mech:madcat","gesture":"Walk","source":"madcat_walk.agl","fallback":"stock"},
  {"type":"fx","replaces":"effect:PPC_Trail","source":"plasma.fx.json","fallback":"stock"}
]}
```
Anim records: add a registry probe before the convention path in `msl.cpp:2608`. FX records: feed
`mc2fx build` or extend `SpecLibrary::Load` with a post-load additive merge.

**Hot-reload:** mirror shader pattern (`MC2_SHADER_HOT_RELOAD`, watch+rebuild in
`gameos_graphics.cpp endFrame`) → `MC2_ASSET_HOT_RELOAD` re-invokes the chokepoints on `.ase`/`.fx`
mtime change (`.agl` cook already mtime-gated → free recook).

| Slice | Effort | Note |
|---|---|---|
| Doc + tool the existing `.ase`/`.agl` drop-in convention | **S** | works now, 0 code. *Smallest valuable.* |
| FX retune via `mc2fx build` + blob overlay (doc it) | **S** | shipped; constant-curve edits |
| `anims.json` additive registry (gesture→file remap) | **M** | clone model_override_registry; probe msl.cpp:2608 |
| FX per-effect additive merge (not whole-blob) | **M/L** | `m_effects` frozen array; needs Register/Adopt surface |
| `.ase`/`.agl` exporter (Blender/GLTF → per-node quat/pos retarget) | **L** | no exporter exists — biggest authoring gap |
| GLTF skinned-anim import | **L** | assimp backend ignores `mNumAnimations` |
| Asset hot-reload (`.ase`/`.fx`) | **M** | reuse shader watcher |

**Biggest gaps:** (1) no `.ase`/`.agl` **exporter** (modders can drop-in but can't author);
(2) FX is whole-blob, no runtime per-effect append; (3) FX default-disabled in beta.

---

## Recommended sequence
1. **PPC flight fix** (Part 1) — the concrete user-wanted win. Data velocity bump + env-gated
   de-curve for direct-fire, keep LRM arc. Fixture A/B + interactive visual confirm.
2. **Moddability S-slices** — document the working `.ase`/`.agl` + `mc2.fx` drop-in paths (0 code),
   so swapping is usable now.
3. **`anims.json` additive registry** (M) — gesture→file remap mirroring
   `model_override_registry.cpp`, the cleanest engine step toward declarative anim swap.
4. Later: FX per-effect additive merge, asset hot-reload, `.agl` exporter.

*Recon by 2 scouts (PPC flight: Explore; moddability: general-purpose). Re-grep all line numbers
before editing. PPC fix touches gameplay/damage timing — confirm the spawn-time-lead design before
coding.*
