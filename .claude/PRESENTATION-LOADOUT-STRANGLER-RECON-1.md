# PRESENTATION-LOADOUT-STRANGLER-RECON-1 — current state + Phase-2 strangler seams

Recon by 3 domain experts (mech-import, skeletal-anim, mission-data) for Phase 2 of the unit-model arc. Read alongside `.claude/UNIT-DATA-MODEL-RECON-1.md` (stat/classification data + jump-gate) and the arc roadmap `docs/superpowers/plans/2026-07-01-unit-profile-arc-roadmap.md`.
★ Line refs from recon were read partly against the ROOT tree (stale) — re-confirm anchors in the canonical `nifty-mendeleev` worktree before editing. Architecture is identical.

## Why modding is painful today — three systemic diseases
1. **Index/position-based, not name-based.** Loadout = positional CSV (97-row strides, magic `(row,col)` offsets, `_DEBUG`-only asserts). Weapons/components identified by compbas ROW INDEX. Icons = atlas COLUMN index (`iconPictureIndex`). Names/encyclopedia = numeric `IDS_` into a separately-built `mc2res.dll`/RSP. Model mount points = node-NAME strings that must exactly match the mesh with no validation. Three+ hand-maintained index spaces that must stay in sync across `.csv`/`.tga`/`.dll`.
2. **Class-tag dispatch repeated everywhere** — same disease as the capability arc. Appearance kind = `switch(appearanceClass)` → one C++ subclass per tag (`apprtype.cpp`). Animation triggers hardcoded in `BattleMech` (`if inJump…`, `setGestureGoal(6)`). Adding a presentation/action kind = new C++ subclass, not data.
3. **Dual-reader divergence.** The SAME chassis CSV is parsed twice by independent readers with different location models: `LogisticsVariant` (UI/mechlab, 3-slot HEAD/CHEST/LEGS enum) and `BattleMech::init(variantNum)` (sim, 8-body-location `bodyLocation`). A unit can load in the mechlab and misbehave in-mission with no diagnostic.

## The good news — a proven strangler pattern already exists
`model_override_registry` and `anim_override_registry` are **additive-merge, gated, CSV-fallback** registries already in the tree. Phase 2 should replicate this shape: new data-driven loader gated `MC2_UNIT_PROFILE_DATA`, falling back to CSV/INI when the def is absent, so **stock stays byte-playable**. Do NOT modify the `mc2srcdata/` submodule as engine work. Do NOT auto-scale the icon-atlas blit math (known canary).

---

## THE CRUX (both loadout & presentation): logical hardpoints don't exist
- Loadout→model mount is a **class-bucketed least-used round-robin**: `Mech3DAppearance::getWeaponNode(weaponType)` scans INI `nodeData[]` by coarse form (Energy/Ballistic/Missile), returns the least-used matching node; `inventory[].facing` stores that node index and **`inventory[].bodyLocation` is set to 255 — the CSV body location is discarded**.
- Mount legality (mechlab `canAddComponent`) = grid-Tetris (component icon dimensions) + 3 hardcoded capability bools (JJ/sensor/ECM forced to sentinel slots) + per-item compbas `criticalSpacesReq[location]` "No" flags. **No typed hardpoint** ("this slot accepts Missile only") anywhere.
- So "this PPC mounts on the right-torso energy socket" is unrepresentable. The logical-hardpoint join key (loadout mount ↔ damage location ↔ model socket) must be **invented**; the interception seams are clean.

---

## Strangler seams per Phase-2 slice

**UNIT-DEFINITION-SCHEMA-1** — umbrella loader.
- Seam: `LogisticsData::initVariants` / `initComponents` (`logisticsdata.cpp:223/116`) — the SINGLE enumeration chokepoint for all placeable types (`buildings.csv` master index). Iterate a `defs/*` registry here *in addition to* `buildings.csv` (additive-merge, like the override registries); rest of the game only sees the resulting in-memory lists.

**PRESENTATION-PROFILE-1** — model/skeleton/sockets from data.
- Seam 1: `AppearanceTypeList::getAppearance` (`apprtype.cpp:225`) — model-selection chokepoint every unit class flows through (replaces the `switch(appearanceClass)` name-string cache).
- Seam 2: `Mech3DAppearanceType::init` (`mech3d.cpp:288`) — the INI manifest parse (`[TGLData]` LODs, `nodeData[]` sockets). PresentationProfile supplies model path/LOD set/socket table declaratively instead of ad-hoc INI reads.
- Adapter host: `GameAdapters/MechRenderAdapter.cpp` already bridges `BattleMech`→`Mech3DAppearance` — natural place for facet→legacy adapter without touching CPU/GPU render.
- Paint/material seam: `resetPaintScheme` (`mech3d.cpp:1968`) — only slot 0 bound today (imported-mesh trap).

**HARDPOINT-SOCKET-VALIDATOR-1** — the crux closer.
- Seams: `getWeaponNode` + `inventory[].facing` assignment (`mech3d.cpp:1045`, `mech.cpp:1647`) for loadout→socket; `mechShape->GetTransformedNodePosition(name)` (`mech3d.cpp:886/1014`) is the socket-resolution primitive; `canAddComponent` (`logisticsvariant.cpp:596`) for typed-hardpoint legality. Validator asserts every logical hardpoint resolves to a DamageSchema location AND a Presentation socket, emitting modder-readable errors.

**UNIT-VARIANT-LOADOUT-1** — 3-layer split (already smeared, unify it).
- ChassisProfile ≈ `LogisticsChassis` (`logisticsvariant.h:30`; capability bools at `:78-84`). VariantLoadout ≈ `LogisticsVariant` (`:113`, `ComponentInfo[54]`). InstanceLoadout ≈ a mutated saved `LogisticsVariant` (no distinct type today). Sim loadout = `Mover::inventory[]` (`mover.h:396-412`), rebuilt from CSV at spawn — NOT handed from LogisticsVariant.
- Seam: `LogisticsChassis::init`/`LogisticsVariant::init` (`logisticsvariant.cpp:42/323`) + `LogisticsComponent::init(dataLine)` / `MasterComponent::initEXCEL` (`cmponent.cpp:87`, has proven `mc2weapon` overlay precedent). Goal: one shared `LoadoutProfile` both the UI reader and the sim reader consume (kills dual-reader divergence). Equipment expressed `Weapon@LogicalHardpoint`, never `Weapon@modelBone`.

**ACTION-ANIMATION-BINDINGS-1** — semantic tags, not class calls.
- ★ Seam A (best first target): `setGestureGoal(int)` (`mech3d.cpp` + all `mech.cpp` callers) — every action already funnels here. PresentationAdapter emits semantic tags (JumpStart/Loop/Land, Move, Death…); an AnimationGraph resolves tag→gesture-id and calls existing `setGestureGoal`. Zero change below the line.
- ★ Seam B: `selectClipForMotion(gesture, motion)` (`assimp_importer.cpp:875`) — ALREADY a nascent resolver (precedence hit-react>knockdown>turn>locomotion, thresholds, hold-at-end). Generalize signature to `(SemanticTag, motion)` + back with a data table.
- Seam C: `AnimOverrideRegistry::resolve` (`mech3d.cpp:580`) — re-key `(mech,gesture)`→`(unitProfile, semanticTag)`.
- FX-event seams (fire/jumpjet are NOT skeletal): weapon-node FX (`setWeaponNodeUsed`, `mech3d.cpp:826`) + `jumpJetEffect`. Semantic "event→FX" edge, separate from "event→clip".
- ★ Do NOT touch below `setGestureGoal`: FK/skinning, `setAnimation(shape, gestureId)`, GPU bone palette — the graph must PRODUCE a valid `(gestureId, frame)`, never rewrite FK.

**UNIT-EDITOR-SURFACE-1** — editor lists/loads/validates `UnitDefinition`s (in + out of mission). Rides on the schema loader + validator above.

**NON-MECH-PROOF-UNIT-1** — stand up a data-only unit (model+loadout+actions+presentation) proving the path isn't class-locked. Phase-2 analogue of the capability proof.

## UIProfile seams (cross-cutting)
`Mover::getIconPictureIndex()` + `LogisticsChassis::iconFileNames[]` + `nameID`/`encyclopediaID` reads (`logisticsvariant.cpp:53/65/68`, `mechicon.cpp`, `mechlopedia.cpp:455`). A UIProfile indirection (name→atlas-slot / name→icon-file / name→string) at these ~4 accessors decouples icons/portraits/strings from the three index spaces. Feed as explicit per-asset overrides; do NOT change the `unitIconX/Y`/`s_MechTextures->width` blit math.

## Guardrails (every Phase-2 slice)
- Gated `MC2_UNIT_PROFILE_DATA`, CSV/INI fallback (stock byte-playable). Additive-merge registry pattern. No `mc2srcdata/` submodule edits. No icon-atlas auto-scale. Re-confirm all line anchors in the canonical worktree.
