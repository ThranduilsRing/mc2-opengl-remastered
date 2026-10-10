<!-- doc-status: current | reviewed:2026-10-11 -->
# Thermal ViewMode — engine-bearing units read hot (THERMAL-VIEW-MECH-HOT-1)

Status: **SHIPPED (mechs).** Slice 2 of `GAMEADAPTERS-VISUAL-STATE-BRIDGE-OPUS-1`.
Vehicles-hot remains a follow-up (see below).

## Corrected design: heat is a CLASS, not a value

Real per-unit heat does **not** exist in this engine — the MechWarrior heat sim
is compiled out (`#ifdef USEHEAT`, never defined; `code/mech.cpp:238`…). So
Thermal does NOT try to read a per-mech heat number. Instead:

> **Anything with an engine (mechs) reads HOT; everything else maps scene
> luminance to the iron palette** (fire/exhaust/muzzle/specular still read warm;
> dark terrain reads cool).

## How it works (no new GPU buffer)

The object-ID buffer is already bound at composite unit 2 (`u_objectIdTex`,
`GL_R32UI`, a `RenderObjectHandle`). Mech handles occupy a **disjoint index
range** — `>= kMechHandleBase` (`0x10000`); static props/terrain are below it
(invariant enforced in `RenderWorld.cpp:122-126`). So a pixel is engine-bearing
iff `(objectId & 0xFFFFF) >= u_engineIdxBase`. No SSBO, no MRT, no per-handle
table.

- `RenderWorld::MechHandleIndexBase()` exposes `kMechHandleBase`.
- `gos_postprocess.cpp` sets `u_engineIdxBase` to it when the OID buffer is live,
  else `0` (Thermal degrades to the prior luminance-only placeholder — no read
  of an unbound texture unit).
- `shaders/postprocess.frag` Thermal branch (mode 3): forces mech pixels into the
  hot band (`t = max(t, 0.9)`) before the iron-palette ramp.

ViewMode-only (`MC2_VIEWMODE_FRAMEWORK=1`, mode 3). Visual mode and the default
path are byte-identical.

## Validation
- Build mclib+mc2 exit 0; deployed exe + `postprocess.frag` (5× `u_engineIdxBase`
  refs); shader-reflect golden regenerated.
- Gate-OFF mc2_01 PASS, +0 destroys (byte-identical).
- Thermal run (`MC2_VIEWMODE_FRAMEWORK=1 MC2_VIEW_MODE=thermal
  MC2_OBJECT_ID_BUFFER=1`) mc2_24 PASS — `[VIEWMODE v1] framework=1
  initialMode=3` active, composite + shader compiled, **0 GL errors**.
- Pixel-level "mechs render orange" is a manual capture (game runs minimized in
  smoke; see `docs/tactical-presentation-visual-state-capture.md`). The
  classification is correct-by-construction from the verified handle ranges.

## Follow-up — VEHICLES-HOT (`THERMAL-VIEW-VEHICLE-HOT-1`)
Vehicles (`GroundVehicle`/`GVAppearance`) render through the **static-prop
batcher** (`gvactor.cpp:1104` → `GpuStaticPropBatcher::registerMultiShape`) and
get a **StaticProp-range handle** indistinguishable from buildings. So vehicles
currently read cool. Making them hot needs object identity:
1. Add `RenderObjectKind::Vehicle` + a `kVehicleHandleBase` carved out of (or
   above) the static-prop range, OR a per-handle "engine" bit table.
2. A `GameAdapters` `registerVehicle` path on `GVAppearance` (mirrors
   `registerMech`).
3. Extend the shader test to include the vehicle range/bit.
This is a separate slice (touches RenderWorld handle allocation + a new adapter)
— out of scope for the mechs-hot MVP.

## Optional future upgrades
- Per-mech damage-driven heat ramp: `MechVisualState.damage01` already bridges
  (Slice 1), but it lives in the per-instance SSBO, not indexable by handle in
  postprocess — would need a per-handle damage table. Defer until wanted.

## Cross-references
- `RenderCore/MechVisualState.h` (Slice 1 bridge).
- `docs/thermal-ir-design.md` (placeholder history + MRT-heat plan).
- `docs/tactical-presentation-visual-state-capture.md` (manual capture steps).
