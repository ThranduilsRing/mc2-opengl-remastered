# UNIT-PROFILE arc — roadmap (post-seam)

**Context:** UNIT-PROFILE-SEAM-1 (capability facet + `canPerform` + `.fit [UnitProfile]` data read + witness dump) is verified working on branch `claude/unit-profile-seam`. This roadmap continues toward the north-star: **data-defined unit facets + registered action executors + schema validation, with existing C++ classes as adapters.** Strangler pattern — NOT an ECS/scripting rewrite.

## North-star shape
```
UnitDefinition            = the modder-facing package (references the facets below; NOT one blob)
  ├─ UnitProfile/Combat   = rules/capabilities (what it is ALLOWED to be)   [static, type-load]
  ├─ LocomotionProfile    = movement mode (mech/tracked/wheeled/hover/infantry/jump)
  ├─ DamageSchema         = hit locations / armor / internal structure
  ├─ HardpointProfile     = HardpointId + allowed weapon tags + damage location + presentation socket + muzzle/fx socket + UI slot  ← THE JOIN KEY
  ├─ LoadoutProfile       = equipment mounted on HardpointIds (what is mounted, where)
  ├─ PresentationProfile  = model/skeleton/animgraph/VFX/audio/sockets (how it looks/sounds/animates)
  ├─ AnimationBindingProfile = ActionAnimTag → clip/gesture (JumpStart/Loop/Land, FirePrimary, Death, …)
  ├─ UIProfile            = icon/portrait/minimap/silhouette (how it appears in menus/HUD)
  └─ ValidationProfile    = construction rules; proves the facets agree before a mission starts
UnitRuntimeState          = what THIS instance currently is (mutable, per-mission)
Action / Executor         = how an allowed behavior actually RUNS (registry-bound)
```
**Rule of separation (keep these apart, join them with stable names):** *capability* (can this unit do X?) · *executor* (what code runs X?) · *loadout* (what is mounted where?) · *presentation* (what model/socket/anim/VFX/audio represents it?) · *UI* (what icon/name/portrait/marker?). Join keys are **stable names, never indices**: `UnitDefinitionId`, `HardpointId`, `ActionId`, `DamageLocationId`, `PresentationSocketId`, `AnimationTag`, `IconId`, `LoadoutId`, `WeaponDefId`. No more "row-97 + atlas-column + DLL-string + least-used-node-bucket." Do **not** make `UnitProfile` the everything-blob — it *references* the facets. Existing `BattleMech`/`GroundVehicle` classes become executors/adapters.

**Namespaced IDs (adopt before this grows):** all stable ids are `namespace:name`, e.g. `core:hunchback_hbk4g`, `core:locomotion_mech_biped`, `core:hardpoint_righttorso_ballistic_1`, `myMod:urbanmech_lam_test`. This gives deterministic multi-mod override/merge rules and is what avoids the next generation of index pain. (Phase-2 `HardpointId` is already a hashed name — namespacing is a name-convention layer on top, formalized in Phase 3.)

**Save/load is NOT a blocker (clarified):** MC2 has no loadable in-mission save/load — only after-action; in-mission quicksaves are not loadable once you exit a mission. So the `InventoryItem::hardpointId` raw-blob size shift (slice 3) is a **non-issue** for real play. `SAVELOAD-VERSIONED-UNITSTATE` (Phase 3) remains good hygiene before this goes public/stable, but it is NOT gating.

**★ HardpointId is the center of Phase 2.** The recon's biggest finding: *logical hardpoints do not exist today* — the loadout slot, damage location, model socket, muzzle/FX, UI slot, and animation event each invent their own index language, joined by nothing. `HardpointId` (e.g. `RightTorso_Energy_1`, `LeftArm_Ballistic_1`, `Infantry_PrimaryWeapon`, `Vehicle_Turret_Heavy_1`) is the one bridge every system maps to. That lets the validator say useful things: *"AC20 on RightTorso_Energy_1: invalid weapon class"* · *"RightTorso_Ballistic_1 has no model socket"* · *"model has socket muzzle_rt_01 but no hardpoint maps to it"* · *"loadout references LeftArm_Energy_2 but damage schema has no LeftArm"* · *"Jump exists but AnimationBindingProfile has no JumpLand fallback."* Never bind a weapon directly to a model bone; a new model only supplies expected sockets. Animation stays action-driven — semantic tags resolve to legacy gesture ids; `setGestureGoal(int)` remains the final legacy sink (do NOT rewrite animation).

## Key correction over the seam
`canPerform(Jump) -> bool` becomes `queryAction(UnitAction) -> ActionAvailability`:
```
Available | MissingExecutor | MissingLocomotionFacet | MissingAnimation
| MissingResource | DisabledByDamage | InvalidForUnitSchema
```
So "profile grants Jump but only BattleMech owns the executor" is *reported*, not silently confusing. The validator yells at load: "UnitProfile grants Jump, but no compatible JumpExecutor is bound."

## Enabling piece FIRST (Phase 0)
**UNITPROFILE-OFFLINE-HARNESS-1** — extend `tests/unit/` (doctest, standalone `build64-tests`, GL/IO-free) to cover the pure logic of every slice below BEFORE engine integration, so iteration is seconds not a 10-min build→deploy→smoke loop.
- Cover: `CapabilitySet` (done), `ActionAvailability` reasoning, `ActionExecutorRegistry` compatibility resolution, `UnitProfile::validate` construction rules, facet composition.
- Rule: keep all new *logic* in leaf TUs with no engine/GL/IO drag so they compile into `mc2_tests`. Engine glue (init sites, dump) stays thin and is verified by smoke.
- Deliverable: red/green doctest cases that a slice's logic must pass before it touches `mech.cpp`/`mover.cpp`.

## Slice sequence (each: offline doctest first → engine glue → gate-OFF parity + gate-ON dump proof)
1. **UNITPROFILE-TYPELOAD-SOURCE-1** — populate `UnitProfile` ONCE at the type/data-load layer, retiring the scattered per-instance-init populates (this session proved that scattering is fragile — the mech populate was dead code after a `return`). Single source of truth.
2. **UNIT-ACTION-QUERY-RESULT-1** — replace `bool canPerform` with `queryAction(UnitAction) -> ActionAvailability`; `canJump()` etc become thin adapters over it. Gate-preserving.
3. **ACTION-EXECUTOR-REGISTRY-1** — registry binding `UnitAction -> Executor` + compatibility rules; jump/move/attack executors registered; queries resolve executor instead of asking "is BattleMech?".
4. **LOCOMOTION-FACET-1** — data-owned movement mode (mech-walker / tracked / wheeled / hover / infantry / jump-capable); locomotion facet gates jump executor compatibility.
5. **DAMAGE-SCHEMA-FACET-1** — mech body parts / vehicle facings / squad members / single actor as a data facet; `resolveHit(HitRequest)` routes through it.
6. **UNITPROFILE-VALIDATOR-1** — load-time rejection of impossible profiles (grants-jump-without-executor, squad-schema-with-mech-UI, etc). The tabletop construction-rules layer, report-then-enforce.
7. **VEHICLE-JUMP-PROOF-1** — data-granted jump on a non-mech using a *compatible executor* (gated/test-mission only). The architecture-no-longer-class-locked proof. Not vehicles-jump-by-default.

Cross-cutting, fold in as reached: **SAVELOAD-VERSIONING** (runtime state serializes by schema, not class), **EDITOR/MOD SURFACE** (expose facets in `.fit`/editor).

## Phase 2 — Presentation + Loadout facet family (after Phase 1)
Ties model/animation/icons/loadout into the SAME facet system (separate facets under `UnitDefinition`, not jammed into `UnitProfile`). Depends on Phase 1's facet framework + validator. Loadout gets its own rework, split into three layers: **ChassisProfile** (what can be mounted) → **VariantLoadout** (default/factory equipment) → **UnitInstanceLoadout** (player's customized/persistent state). Out-of-mission `RosterUnit/HangarUnit` and in-mission `MissionUnit` consume the same validated definition.

Slice order (HardpointId-first; each slice = gated data facet + optional read + validate + **legacy fallback untouched**, ONE seam at a time — NOT a rewrite of mechlab/animation/appearance):
1. **HARDPOINT-ID-SCHEMA-1** — define `HardpointId`, weapon tags, damage-location link, socket-name link. No behavior change; pure schema + doctest.
2. **PRESENTATION-SOCKET-PROFILE-1** — optional socket table loaded from data; validator checks socket names against model/appearance metadata where possible. Legacy INI `nodeData[]` fallback untouched. Seam: `Mech3DAppearanceType::init` / `GetTransformedNodePosition`.
3. **LOADOUT-HARDPOINT-JOIN-1** — loadout entries may name a `HardpointId`; fallback to legacy positional CSV when absent. Seam: `LogisticsData::initVariants` (additive-merge like model/anim override registries).
4. **GETWEAPONNODE-HARDPOINT-ADAPTER-1** — route `Mech3DAppearance::getWeaponNode()` through `HardpointId → socket`; fallback to current least-used round-robin when no profile. Returns the same node index the runtime already consumes (no downstream render change).
5. **ACTION-ANIMATION-BINDINGS-1** — `ActionAnimTag` (JumpStart/Loop/Land, FirePrimary, Death, HitReaction, …) resolves to legacy gesture id; `setGestureGoal(int)` stays the final legacy sink. Seam A = above `setGestureGoal`; generalize `selectClipForMotion`. Do NOT rewrite FK/skinning.
6. **UI-PROFILE-1** — unit icon/minimap/silhouette/portrait via named profile; fallback to old atlas/index. Seams: `getIconPictureIndex`, `iconFileNames[]`, name/encyclopedia IDs. Do NOT auto-scale the atlas blit math.
7. **SINGLE-SOURCE-UNIT-DEFINITION-1** — mechlab/logistics and mission sim read the SAME `UnitDefinition` facade (kills the dual-reader divergence: `LogisticsVariant` UI vs `BattleMech` sim).
8. **UNIT-DEFINITION-VALIDATOR-1** — fail missing sockets, bad hardpoints, bad damage locations, missing icon, missing executor, missing animation fallback — in modder terms, at load.
9. **NON-MECH-DATA-UNIT-PROOF-1** — test-only unit proving loadout + model socket + action + UI are not class-locked. The Phase-2 analogue of the capability proof.

**Redlines (guard against a giant rewrite):** GOOD = add gated profile registry, read optional facets, validate, fallback to legacy CSV/gesture/node, replace ONE class/index seam at a time. BAD = rewrite mechlab, rewrite the animation system, replace all appearance classes, or convert every unit to a new schema in one slice. Re-ground every line citation in the canonical `nifty` worktree (recon partly read root).

Modder north-star workflow (the acceptance test for Phase 2):
```
mods/MyUnit/  unit.fit  presentation.fit  hardpoints.fit  loadouts/HBK-4G.fit
              models/hunchback.glb  icons/hunchback.png  anim/mech_biped.animgraph
→ run unit validator → unit appears in editor/mission
```
Validator must yell in modder terms, e.g.: `Missing socket: RightTorso_Ballistic_1` · `Weapon AC20 requires Ballistic hardpoint` · `Jump granted but no JumpExecutor-compatible locomotion` · `AnimationGraph missing JumpLand` · `Damage location RightArm referenced but schema has no RightArm`. That is the line between "mod-friendly" and "modders grep C++ crashes."

## Phase 3 — Modder-facing UnitDefinition (after Phase 2)
Phase 1 made the engine stop asking *"what C++ class is this?"*; Phase 2 made presentation/loadout stop asking *"what hardcoded index/bucket is this?"*. Phase 3 makes the whole thing a **modding product**: a modder defines a unit, gives it movement/damage-locations/hardpoints/weapons/model+sockets/icons, runs a validator, and it appears in logistics + editor + mission — never thinking about `BattleMech`/`Mover`/CSV-row-offsets/gesture-ints. The meta split: *HardpointId solves joins · UnitDefinition solves packaging · validator/modlint solves usability · legacy synthesis solves compatibility · single-reader facade solves UI-vs-mission divergence · versioned UnitState solves save/load hygiene · preview tooling solves asset iteration.*

Slices:
1. **UNIT-DEFINITION-FACADE-1** — one top-level `UnitDefinition` data object references all facets (Identity/UnitProfile/Locomotion/DamageSchema/Hardpoint/Loadout/Presentation/AnimationBinding/UI/AI/ConstructionRules).
2. **LEGACY-UNIT-DEFINITION-SYNTHESIS-1** — stock CSV/FIT units generate a SYNTHETIC `UnitDefinition` facade at load → old content flows through the SAME validator/query/presentation path as new content (compatibility without hand-conversion).
3. **UNIT-MODLINT-CLI-1** — validate a unit folder WITHOUT launching the game (`mc2_modlint --unit mods/MyMod/units/x.fit` → the modder-term errors above). The validator becomes a modder tool, not just runtime safety.
4. **UNIT-PREVIEW-DUMP-1** — headless/model-aware report: model loaded, socket count, hardpoints resolved, missing sockets, animation tags available, icon paths resolved. CLI first, editor UI later.
5. **LOADOUT-THREE-LAYER-1** — split ChassisProfile (mount capacity/mass/structure/allowed categories) / VariantLoadout (stock config) / InstanceLoadout (player-customized/persistent) — supports stock/custom/campaign-persistence/salvage/mechlab/spawn without mixing "can mount" vs "currently mounted".
6. **UNIT-ID-NAMESPACE-1** — `modId:name` stable ids everywhere + deterministic override/merge rules.
7. **LOGISTICS-MISSION-SINGLE-READER-1** — logistics UI and mission spawn consume the SAME `UnitDefinition` facade → kills dual-reader divergence (the "loads in mechlab, breaks in mission" failure). (This is the Phase-2 SINGLE-SOURCE slice's full realization.)
8. **SAVELOAD-VERSIONED-UNITSTATE-1** — versioned UnitInstance serialization so runtime/loadout state stops relying on raw blob layout. NOT gating (MC2 has no loadable in-mission save/load), but right before public/stable.

### Format north-star: BT2018-shaped, MC2-shaped (familiar to modders)
Reference the actual HBS BattleTech (2018) asset dump `A:\Games\mc2-opengl\BattleTech_2018_Dump` (Animator / TextAsset / Mesh / MonoBehavior) when shaping the `UnitDefinition` file format — modders already know it, MC2 already imports BT2018 mechs via mod, so aligning the def shape gives porters an easy path. Goal is *easy modding à la BT2018 / MW5 Mercs*, "BT2018-shaped but MC2-shaped." Facet mapping to their schema (from the early TextAsset recon): `chassisdef → ChassisProfile/DamageSchema/Hardpoints`, `mechdef → VariantLoadout`, `movedef → LocomotionProfile`, `hardpointdatadef → PresentationProfile sockets`, everything-is-a-Component → LoadoutProfile items, `Animator → AnimationBindingProfile`. Few wrong answers here — optimize for modder familiarity + validator clarity.

### BT2018 format lessons (recon `.claude/PRESENTATION-LOADOUT-STRANGLER-RECON-1.md` + BT2018 dump) — steal these 3
1. **String-ID reference graph, never positional/index.** Every join in BT2018 is a greppable readable string (`ChassisID`, `HardpointDataDefID`, `ComponentDefID`, `PrefabIdentifier`). Our `UnitDefinition` format + `modId:name` ids must do the same — kills MC2's positional-CSV/atlas-column/IDS-number pain at the root.
2. **`<mount><index>` hardpoint-slot naming convention** (`_bh1`/`_eh1`/`_mh1`/`_ah1` = Ballistic/Energy/Missile/AntiPersonnel slot N) shared across mesh ↔ weapon-prefab ↔ hardpointdatadef, so the mesh self-describes which slot it fills — no external mapping table. This is the concrete shape our `HardpointId` + PresentationSocket naming should adopt.
3. **Chassis/variant separation.** BT2018 splits: `chassisdef` (fixed frame: locations, armor/structure, `Hardpoints[]` mount types) + `hardpointdatadef` (location→slot→visual-prefab list, shared across variants) vs `mechdef` (per-variant `inventory[]` loadout) + `movedef` (per-variant locomotion). Directly maps to our Phase-3 `LOADOUT-THREE-LAYER-1` (Chassis/Variant/Instance). Adding a variant = one small file referencing an existing frame.

Hardpoint 3-file join to mirror: `chassisdef.Locations[].Hardpoints[] {WeaponMount: Ballistic|Energy|Missile|AntiPersonnel, Omni}` (mount-type legality) × `hardpointdatadef` (slot→prefab options) × `mechdef.inventory[] {ComponentDefID, MountedLocation, HardpointSlot}` (loadout). `Weapon.Category` must match a location's `Hardpoint.WeaponMount` — validatable at author time (exactly our `hardpointAcceptsWeapon` + the future HARDPOINT-ENFORCEMENT-1). Animator is BINARY (no text state machine) → AnimationBindingProfile stays MC2-shaped (explicit `ActionAnimTag→clip+fallback` over Assimp named clips — already built in Phase-2 slice 5).

### Hardpoints: today firing-location only; ENFORCEMENT is a future slice
★ Current MC2 (even post-Phase-2): hardpoints are a *firing location*, NOT a component filter — they do not restrict what can be mounted. Our `HardpointProfile.acceptedTags` + `hardpointAcceptsWeapon` + validator `UPV_HARDPOINT_WEAPON_CLASS_REJECTED` currently only **report** a mismatch. Making typed hardpoints actually **restrict** mounting (mechlab rejects an AC20 on an energy hardpoint, BT2018-style) is a dedicated future slice **HARDPOINT-ENFORCEMENT-1**. ★**BLOCKED / NOT NOW (user, 2026-07-01):** the mech bay UI is not set up to support mount restriction yet — do NOT enforce. Stay report-only until the mechlab can present/handle typed slots. Report-first → enforce-later, and only after the def format is authoritative + never on legacy stock loadouts built without hardpoint typing.

### Equipment-definition arc (TEED UP — reframed per advisor 2026-07-01)
★ **`compbas.csv` is the COMPONENT/EQUIPMENT table, not "weapons forever".** Build `EquipmentDefinition` (with optional facets), NOT a bare `WeaponDefinition` — else we solve lasers/ACs/missiles and rediscover the same 255-cap for ammo/sensors/ECM/jumpjets/specials. **`WeaponDefinition = EquipmentDefinition with a WeaponFacet.`** (The Phase-0 `code/weapon_definition.h` becomes the WeaponFacet content — not wasted.) Scope doc: `.claude/WEAPON-DEFINITION-ARC-SCOPE-1.md` (reframe to equipment).
```
EquipmentDefinition
  ├─ identity (modId:name) + tags + kind ; legacyMasterId = compatibility ALIAS (NOT identity)
  ├─ WeaponFacet     (optional): fireExecutor + damage/heat/recycle/range/cooldown/CV
  ├─ AmmoFacet       (optional): named ammoCategory (not WEAPON_AMMO_OFFSET arithmetic)
  ├─ SensorFacet / ECMFacet / JumpJetFacet / … (optional)
  ├─ Heat/energy/cost/tech/era metadata
  ├─ PresentationProfile (VFX/SFX, event-based — see below)
  └─ Validator
```
**Design rules (from advisor):**
- **Behavior ≠ presentation.** `WeaponFacet` = mechanics; `WeaponPresentationProfile` = look/sound. Lets a modder do same-mechanics/different-visuals (and vice-versa) with no C++.
- **String-first identity.** Canonical id = `modId:name` (e.g. `core:medium_laser`, `core:ac20`, `myMod:clan_rotary_ppc`); hash = impl detail; **`legacyMasterId` is a compatibility alias, NEVER the identity** (mod load order must not change equipment identity).
- **Fire-executor registry** — do for weapons what the unit arc did for jump: `fireExecutor = "beam_direct"|"projectile_ballistic"|"missile_salvo"|"artillery_indirect"|"flamer_area"|"tag_designator"|…`; gameplay asks `weaponRegistry().executor(def.fireExecutor).fire(FireRequest{...})`, not `if (masterId==PPC || class==LASER)`. Schema-only early; DO NOT rewrite firing.
- **Event-based presentation.** Executor emits semantic events (`OnFire`/`OnProjectileSpawn`/`OnMuzzleFlash`/`OnBeamStart`/`OnBeamEnd`/`OnImpact`/`OnMiss`/`OnJam`/`OnAmmoDepleted`/`OnOverheat`); PresentationProfile maps events→assets (sfx/vfx/decal/light). Never hardwire SFX/VFX into the executor.
- **Snap into the unit spine:** `WeaponTag on weapon ↔ allowedTags on HardpointProfile ↔ HardpointId in loadout ↔ socket/muzzle in PresentationProfile` — the author-time legality check (validator messages: *"core:ac20 requires Ballistic hardpoint; mounted on RightArm_Energy_1"*, *"core:medium_laser has muzzleVfx but hardpoint has no muzzle socket"*, *"myMod:plasma_rifle references missing fireSfx sfx:plasma_rifle_fire"*).
- **Future facet (not now): `DamageEffectProfile`** (cluster/area/heat/stability/knockdown/sensor/ecm/AP/crit-bonus/splash/building) — start with exact stock fields, add later.
**★ Kill the 255 cap SAFELY via shadow path, not by ripping out compbas:** compbas.csv → converter → EquipmentDefinition registry → shadow-compare → legacy `MasterComponent` still feeds runtime → later gated source flip. Note the deeper cap: `inventory[].masterID` is `unsigned char` (8-bit) baked into loadout+save — full lift needs widening that field (save/load is a non-issue per user, so OK — but a deliberate step). Acceptance before flip: stock row count converts · all legacy masterIDs map to string ids · all defs validate · stock stats shadow-match · VFX/SFX refs resolve-or-waived · mission+mechlab smoke parity.
**Reuse the unit spine wholesale:** `WeaponTag`+`hardpointAcceptsWeapon()` (built) · `tools/unit_converter/`+`tools/mc2_modlint/` (generic — extend) · `unit_profile_validator.h` framework · STANDARD SLICE-DISPATCH CONTRACT (gated `MC2_WEAPON_DEFINITION_DATA` default-OFF, legacy fallback, stock-parity, STOCK-ONLY). Source gate `MC2_WEAPON_DEFINITION_SOURCE=legacy|generated`.
**Sequence — TOOLING HALF DONE:** 0 harness `d90db3c9` · 1 EQUIPMENT-DEFINITION-SCHEMA `01dd7110` · 2 WEAPON-FACET+EVENT-PRESENTATION `7a63d5d1` · 3 (merged into 2) · 4 LEGACY-COMPBAS-CONVERTER `6a13f06a` · 5 WEAPON-SHADOW-COMPARE `75a6278f` · 6 WEAPON-VALIDATOR `903cc55f` · 7 WEAPON-MODLINT `9f8c8c51`.

**Engine half — RUNTIME IDENTITY SURGERY (advisor-refined; recon-first, NEVER registry-facade+widen in one slice):**
- **E1 EQUIPMENT-MASTERID-USAGE-RECON-1** (READ-ONLY first): map EVERY place equipment identity is treated as `uint8_t`/BYTE/char/0–255 index/array offset/CSV row ordinal/`MasterComponent*`/`inventory[].masterID`/packet-save-blob field/UI index/firing lookup/ammo lookup. Determines whether the widen is surgical or spiderwebbed. NO code change.
- **E2 EQUIPMENT-REGISTRY-FACADE-1**: runtime can load/query `EquipmentDefinition` by qualified id AND `legacyMasterId`. NO masterID widening. Legacy `MasterComponent` stays authoritative. Gated.
- **E3 EQUIPMENT-REF-TYPE-1**: introduce `EquipmentRef`/`EquipmentOrdinal` typedefs; replace raw `masterID` signatures where easy. Storage UNCHANGED.
- **E4 INVENTORY-MASTERID-WIDEN-1**: the actual storage widen (8-bit `inventory[].masterID` → wider). Gated + shadow-compare + mission smoke. NO firing rewrite. (Save/load a non-issue per user.)
- **E5 LOADOUT-EQUIPMENT-ID-ALIAS-1**: loadouts may reference `core:medium_laser`; legacy ordinal fallback remains.
- **E6 EQUIPMENT-SOURCE-FLIP-SHADOW-1**: gate `MC2_WEAPON_DEFINITION_SOURCE=legacy|generated`; compare before generated becomes authoritative.
- **E7 FIRE-EXECUTOR-REGISTRY-1**: runtime resolves `fireExecutor`; legacy firing still authoritative — schema/dispatch only.
- **E8 ONE-WEAPON-PRESENTATION-PROOF-1**: one stock weapon routes VFX/SFX through the data profile, parity checked.
★REDLINE: do NOT rewrite weapon firing / ammo resolution / make generated authoritative / enforce hardpoints / remove compbas early. Strangler: legacy works → registry loads beside it → aliases prove equivalence → runtime gradually asks the new registry → only then flip source.

**Post-foundation milestone sequence (advisor 2026-07-01, after E8):** E8 done ("First weapon presentation event resolved from EquipmentDefinition data, legacy firing still authoritative" — NOT "data-driven firing") → **UNIT-EQUIPMENT-FOUNDATION-TIER1-1** (bank a fresh full tier1 across the ~41-slice changeset: full doctest/pytest + unit roster gate + equipment shadow-compare + unit/equipment modlint + preview-dump + mission tier1 + gate-OFF parity) → **UNIT-SOURCE-FLIP-SWITCH-1** (`MC2_UNIT_DEFINITION_SOURCE=legacy|generated`, DEFAULT LEGACY — wire the switch visible+testable, do NOT flip default; honest: jump/loco/dmg/loadout green, hardpoints/sockets/UI partly waived) → **EQUIPMENT-SOURCE-FLIP-SWITCH-1** (`MC2_EQUIPMENT_DEFINITION_SOURCE=legacy|generated`, DEFAULT LEGACY; registry/id-alias/executor/presentation proven, legacy firing authoritative, widen deferred) → **STOCK-GENERATED-SOURCE-SHADOW-RUN-1** (run with generated authoritative behind the gate, shadow-compare before trusting) → **MCO/MC2X-CONVERSION-RECON-1** (the extensibility acceptance test — separate mech/component/brain lists; only AFTER stock is boring) → **INVENTORY-EQUIPMENTREF-WIDEN-1** (=E4; ONLY when live content >254 rows; special care: 0xff sentinel collision, `weapon.ammoMasterId`, `.mdf` disk format, MP/save packet fields, UI index assumptions). Redlines: don't rewrite firing; don't widen just because theoretically needed; don't make generated authoritative before the shadow run; don't enforce hardpoints until mech bay; don't bury the deployed-vs-source compbas divergence.

**★ Support ALL component families incl. default-OFF ones (user 2026-07-01):** stock has many components disabled-by-default (gyroscopes, engines, armor types, heatsinks, …) that are **played with via mods**. The `EquipmentDefinition` schema/converter/registry must cover EVERY compbas kind, not just active weapons — a disabled-in-stock component is still a valid `EquipmentDefinition` a mod can enable. Do not drop non-weapon/disabled kinds.

**★ MOD CONTENT = the post-stock validation TEST (user 2026-07-01, don't build yet):** MCO and MC2X mods ship SEPARATE mech lists / components / brains (they base off stock but add/replace). Once the STOCK conversion (unit + equipment) is solid, converting the MOD content is the real-world proof that the registry/converter/validator actually deliver moddability — a mod adding a 256th component, its own weapon list, etc. is precisely what the ordinal-cap kill exists for. This is the extensibility acceptance test, NOT current work — stay STOCK-ONLY until stock is done, then point the converter at mod content (via `--stock-profile`/a mod path) as the validation milestone. (Brains are the separate TechBrain arc; note the cross-over when reached.)

**★ Canonical stock = source-tree `mc2srcdata` (user 2026-07-01):** `mc2srcdata/objects/compbas.csv` is the canonical stock (edits + mods base off it). The DEPLOYED install compbas (e.g. v0.3) = a compatibility/import profile — the 4 shadow-compare divergences (`Simple` vs `Optics`) are that deployed-vs-source drift, waived/documented, NOT a bug, and must NOT block the registry facade. Future: converter `--stock-profile source|deployed-v03`. Resolve the drift before the FINAL source flip, not before the facade.

### Source-data conversion — STAGED SHADOW, never a one-shot flip (start only after Phase-2 slices 8+9 are green)
The point where the new format is coherent enough to generate INTO is *after* the validator (slice 8) + non-mech proof (slice 9). Then convert stock data yourself — but safely: **generate → validate → shadow-compare → gate the source-of-truth → flip when boring.** Never replace the legacy source in place.
- **LEGACY-UNIT-CONVERTER-1** — read stock mech/vehicle CSV/FIT/INI/icon/model metadata, EMIT sidecar `UnitDefinition` files (`generated/units/core/<unit>/{unit,presentation,hardpoints,loadout,ui}.fit`). Do NOT change the runtime source of truth. Run the validator on the output; produce a conversion report. (Offline tool; distinct from the in-memory `LEGACY-UNIT-DEFINITION-SYNTHESIS-1` facade — the converter emits files, synthesis builds a runtime facade.)
- **UNIT-DEFINITION-SHADOW-COMPARE-1** — for every stock unit, diff legacy-derived runtime data vs the generated `UnitDefinition` across ALL facets (capabilities, locomotion, damage schema, loadout, weapon tags, hardpoints, model path, socket names, UI icon/name/portrait, animation fallbacks, validator findings). Acceptance: stock unit count matches · all generated defs validate · no missing model/icon/socket refs · legacy-vs-generated semantic diff is zero (or explicitly waived) · mission + logistics smoke parity clean.
- **Source gate `MC2_UNIT_DEFINITION_SOURCE=legacy|generated`** — default **legacy**; flip to **generated** only once shadow-compare is boring (all-green). Middle state, allowed EARLIER (after slice 9): **new mod units may use native `UnitDefinition` while stock stays legacy/synthesized** — modders get the clean path without risking stock content.
- **Blunt readiness rule** — convert only when the new format answers ALL of these WITHOUT legacy guessing: what unit is this? how does it move? what damage schema? what hardpoints exist? what weapons mounted where? what model/socket per hardpoint? what UI icon/name? what animation tags/fallbacks? does the validator pass? Phase 2 gets us almost there; the converter starts after 8+9.

Cross-cutting later (multi-mod): **MOD-PACKAGE-MANIFEST-1** (`mod.json`: modId/version/author/deps/loadOrder/unitDefinitions/assets/overrides), **MOD-LOAD-ORDER-1**, **MOD-ASSET-NAMESPACE-1** — deterministic merge once multiple mods define units/weapons/icons/models.

**Animation fallback tree (AnimationBindingProfile detail for Phase-2 slice 5 + Phase-3):** semantic tags resolve through a fallback chain, e.g. `JumpLand → JumpLoop → Move → Idle`, `FireWeapon_Heavy → FireWeapon → Idle`, `Death_Explode → Death → Idle`. Validator WARNs if a fallback chain ends in nothing.

## Standard slice-dispatch contract (applies to EVERY slice; subagents get "implement SLICE-X per roadmap + recon, standard contract")
- **Worktree:** `A:/Games/mc2-opengl-src/.claude/worktrees/unit-profile-seam`, branch `claude/unit-profile-seam`. Root `code/` is STALE — never edit it. `preflight_fast` is hardwired to the nifty worktree and will falsely report `branch_ok=false`; verify instead with `git -C <worktree> rev-parse --abbrev-ref HEAD` == `claude/unit-profile-seam`.
- **Edits:** grep-anchored (line numbers drift). Full relink on header/layout change. Don't touch the stock `case COMPONENT_FORM_JUMPJET` no-break fall-through, or the FK/skinning below `setGestureGoal`.
- **Gate:** everything new behind `MC2_UNIT_PROFILE_DATA` (via `mc2UnitProfileDataEnabled()`), default OFF. Gate-OFF must be behavioral parity. run_smoke propagates the gate (fixed `cfa2f292`).
- **★ STOCK ASSETS ONLY:** any data extraction/conversion/comparison reads base `mc2srcdata` stock data only — NEVER MCO / MC2X / BT2018 / any `mods/` overlay. Engine runs use `--profile stock` (run_smoke default). Never edit `mc2srcdata/`. (BT2018 dump is a format *reference* only, never a conversion source.)
- **Offline-first:** any pure logic goes into `tests/unit/` doctest (standalone `build64-tests`, GL/IO-free), RED→GREEN, BEFORE engine glue. Reuse the Phase-0 contracts (`unit_action.h`, `action_executor_registry.h`).
- **Verify (single mission mc2_01 ONLY — never full tier1 per slice):** commit code FIRST (survives a process death). Build (VS cmake `--target mc2`) → deploy v0.3 ONLY (`--build-dir build64/RelWithDebInfo`; build `mc2_launcher` first if deploy complains) → gate-ON `--mission mc2_01 --no-lease --exe A:/Games/mc2-opengl/mc2-win64-v0.3/mc2.exe` with `MC2_DEBUG_STATE_DUMP=1 MC2_DEBUG_STATE_DUMP_HISTORY=1`: confirm 4 jet-mechs `can_perform_jump=TRUE` in `v0.3/debug_state/history_*.json` → gate-OFF same mission: PASS, Δdestroys +0.
- **Commit:** prefix `feat/refactor/test(unit-profile):` + trailer `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`. Update `.claude/unit-kind-classcheck-audit.md` when a class-check is retired/isolated.
- **Report terse** (keep main context lean): what changed, commit SHAs, build result, gate-ON true-count, gate-OFF parity, any decision future slices must honor. NEVER fake build/smoke output.
- Full tier1 is banked ONCE at arc end before any merge — not per slice.

## ★ SOURCE-FLIP DONE — generated-authoritative dogfood is now a real toggle (2026-07-01)
Both switches wired + engine CONSUMES generated (default LEGACY): `UNIT-SOURCE-FLIP-SWITCH-1` `baa79ee8` (unit loco/damage/jump from `data/defs/units/`), `EQUIPMENT-SOURCE-FLIP-SWITCH-1` `0c3f1c54` (green equipment stats overlaid onto masterList from `data/defs/equipment/`, firing untouched). `STOCK-GENERATED-SOURCE-SHADOW-RUN-1` = **tier1 5/5 PASS Δ+0 with BOTH gates=generated ACTIVE** (185 UNIT + 65 EQUIP source-flip lines, all match-legacy=YES) → generated-authoritative == legacy gameplay across the full roster.
**To dogfood "run it this way":** deploy the generated defs (converter `--batch` → `data/defs/{units,equipment}/`), then set `MC2_UNIT_PROFILE_DATA=1 MC2_UNIT_DEFINITION_SOURCE=generated MC2_EQUIPMENT_DEFINITION_SOURCE=generated`. Unset any = instant legacy fallback. Waived facets (hardpoints/sockets/UI on units; firing/presentation on equipment) stay legacy either way. DEFAULT stays legacy — do NOT flip the default until the waived facets are covered + a longer soak.
**NEXT:** dogfood a while → `MCO/MC2X-CONVERSION-RECON-1` (extensibility acceptance test — separate mech/component lists, the ordinal-cap payoff) → `INVENTORY-EQUIPMENTREF-WIDEN` (E4) only when live content >254 rows.

## Honest promise
today = data-toggled capabilities on existing types; near-term = class-check removal + mod-visible flags; real future = new archetypes with data-defined locomotion/damage/actions/presentation.
