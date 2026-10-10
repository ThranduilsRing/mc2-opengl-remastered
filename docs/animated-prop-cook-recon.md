<!-- doc-status: current | reviewed:2026-10-11 -->
# Animated Prop Cook Recon

**Goal:** Understand what a NODE_ANIMATED_PROP cook should look like for Path A
(preserve legacy runtime animation, modernize geometry/material payload).

**Status:** Recon-only. No code changes.

---

## Meshdump Structure: Animated vs Static

Animated prop meshdumps (`artillerypiece`, `bturretcontrol`, `autocannoncamo1`) have
the same flat structure as static prop meshdumps:

- **Flat submesh list** — no node hierarchy encoded in the meshdump
- **Vertex format**: `[x, y, z, nx, ny, nz, u, v]` (8 floats, same as static)
- **No animation channels** — zero keyframes
- **No named-node data** — node names are in the INI and the TGL shape, not the meshdump

Static reference (`ammodump`, `bunker`) is structurally identical. The meshdump
format does not distinguish animated from static. The classifier difference is purely
INI-driven (`AnimationNodeId`).

**Implication for cook:** the meshdump alone is insufficient to produce a node-split
GLB. A v1 animated prop cook must also read the INI to discover node names, and
the source GLB (if available) or TGL shape for per-node geometry split.

---

## Source GLB Node Hierarchy

No pre-cooked source GLBs exist for animated props in the current cook library.
`A:/Games/mc2-cook-batch/cook-full-out/` contains only STATIC_RENDER_ONLY assets
(animated were skipped by classifier). No separate animated-prop GLB source dir found.

**For v1 cook**, the geometry source is the meshdump (which has all verts flat), and
node assignment must come from either:
1. The TGL shape hierarchy (available at runtime, not offline), or
2. A manually authored or workbench-exported per-node GLB.

Workbench `--export-tgl-meshdump-all` produces flat dumps. A separate
`--export-animated-prop` export mode does not yet exist.

**Gap:** no offline node-split geometry source for automated cook. See Gaps section.

---

## Runtime Animation Path

Source: `mclib/bdactor.cpp`, `mclib/bdactor.h`.

### Step 1 — Type-level: INI → string name

`BldgAppearanceType::init()` (~bdactor.cpp:559):
```cpp
result = iniFile.readIdString("AnimationNodeId", rotationalNodeId, 24);
```
`rotationalNodeId` on the TYPE is a `char[TG_NODE_ID]` fixed buffer.
Value "NONE" = no rotation. Any other value = name of the node to rotate.

### Step 2 — Instance-level: string name → int index (lazy cached)

First time `update()` or `draw()` uses the node (~bdactor.cpp:695-703):
```cpp
if (rotationalNodeId == -1) {
    if (S_stricmp(appearType->rotationalNodeId, "NONE") != 0)
        rotationalNodeId = bldgShape->GetNodeNameId(appearType->rotationalNodeId);
    else
        rotationalNodeId = -2;
}
```
`rotationalNodeId` on the INSTANCE is a `long` (int index or -2 sentinel).

### Step 3 — Per-frame: apply quaternion rotation to named node

~bdactor.cpp:703-704 (and several other sites for dust/death effects):
```cpp
if (rotationalNodeId >= 0)
    bldgShape->SetNodeRotation(rotationalNodeId, &torsoRot);
```

`torsoRot` is computed from gameplay (turret yaw targeting). This is purely
procedural — no keyframes. Works on any shape that has the named node.

### Step 4 — `bdAnimData[]` path (separate from rotationalNodeId)

`bdAnimData[i]` drives gesture keyframe animations (destroyed states, etc.) via
`TG_AnimateShape`. Populated from `[Animation:0]`..`[Animation:N]` INI sections.
Not the same as `rotationalNodeId`. The 29 NODE_ANIMATED_PROP appearances all use
the `rotationalNodeId` path, not `bdAnimData` (which is why `bldgTypeHasAnimations()`
previously missed them).

### Vehicle path

`mclib/gvactor.cpp` (`GVAppearance`) uses `rotationalNodeIndex` (int) identically —
same lazy-cache + `SetNodeRotation` pattern.

---

## TGL Shape Node Identity

Source: `mclib/msl.cpp`, `mclib/msl.h` (`TG_MultiShape`).

Nodes stored as `TG_TypeNodePtr* listOfTypeShapes` (flat array of type-node structs).
Each carries:
- `myType->getNodeId()` → `const char*` name (e.g. "turret", "base")
- `baseRotation` (UnitQuaternion, set by `SetNodeRotation`)
- Parent relationship implicit (scene graph via shape hierarchy)

`GetNodeNameId(const char* name)` does linear scan with `S_stricmp`.
`SetNodeRotation(int idx, UnitQuaternion* rot)` writes to `listOfTypeShapes[idx].baseRotation`.

**Key point:** node lookup is by name string, not by position. Any shape type that
exposes named nodes with the right string will work — TGL or GLB, as long as the
importer populates the node list with the right names.

---

## Recommended Cook Payload v1

**Path A premise:** keep legacy `rotationalNodeId` as animation authority.
Cook provides modern geometry + materials. Runtime applies rotation identically.

### Minimum required GLB structure

```
root (unnamed)
└── <base_mesh_node>        // static base geometry (barrel, pedestal, etc.)
    └── <animation_node>    // node name MUST match INI AnimationNodeId
        └── <turret_mesh>   // rotating geometry (gun barrel, radar dish, etc.)
```

- Node names must match exactly (case-insensitive match already in runtime)
- NO glTF animation channels — runtime applies procedural rotation
- Materials: PBR albedo (KTX2 or TGA), resolved per existing cook registry
- Single LOD (v1) — LOD chain deferred

### What runtime gets

1. GLB loaded via `assimp_importer.cpp` → populates `TG_MultiShape` node list
2. `GetNodeNameId("turret")` finds the node → `rotationalNodeId` cached
3. `SetNodeRotation(idx, &torsoRot)` applies per-frame yaw — unchanged
4. Geometry + materials render via cooked GL path

### What cook must do (not yet implemented)

1. Read INI → get `AnimationNodeId` value (e.g. "turret")
2. Split meshdump geometry by node assignment — BUT meshdump is flat (see Gap 1)
3. Build GLB with named nodes: base + rotation_node
4. Resolve materials per existing KTX2 cook
5. Emit `animated_prop_manifest.json` (schema TBD, separate from static manifest)
6. Do NOT emit a `renderOnly: true` static override entry

---

## INI Fields Survey — 29 NODE_ANIMATED_PROP appearances

Scanned `A:/Games/mc2-opengl/mc2-win64-v0.4/data/tgl/*.ini` for `AnimationNodeId != "NONE"`.

### AnimationNodeId distribution

| Value | Count | Sample appearances |
|---|---|---|
| `turret` | ~15 | gaussturret, lrmturret, srmturret, laserturret, heavyautocannonturret, mediumautocannonturret, clanerlaserturret |
| `TankGTurret` | ~8 | acv, alacorn, bulldog, jenner, locust, tiger, urbieclan |
| `Artillery_Turret` | 2 | artilleryturret, longtomturret |
| `Mog_Turret` | 2 | moglturret variants |
| `sc_turret` | 1 | shadowcat variant |
| `lrmc_turret` | 1 | longtom variant |

All 29 have **exactly one** rotating node (single AnimationNodeId per INI).
No multi-axis animated props observed.

### Secondary INI fields

- `WeaponNodeId0`..`WeaponNodeId3`: attachment points on ~8 props. Non-rotating.
  Include as named nodes in cooked GLB for completeness; do not rotate.
- `[Animation:X]` gesture sections: present in ~4 props. Keyframe data for
  destroyed/damaged states. These use `bdAnimData[]` path, not `rotationalNodeId`.
  **v1 skip**: runtime loads keyframes from legacy TGL fallback when present.
- `ShadowName`: separate TGL for shadow; irrelevant to cook.

---

## Gaps and Unknowns

### Gap 1 (BLOCKER for v1 cook): no offline node-split geometry source

Meshdumps are flat — all geometry in one submesh list with no per-node assignment.
The TGL shape has the hierarchy but is not exported by the workbench meshdump path.

**Options:**
- A: Add a `--export-animated-prop` workbench mode that exports per-node geometry separately
- B: Use the source TGL files directly offline (requires TGL parser in Python cook)
- C: Manually author base+turret GLBs for the 29 props (viable for v1 given small count)
- D: Cook flat GLB (all geometry together) and tag the rotation node as a named empty node; runtime rotation still works, but base and turret share one mesh (visually wrong)

**Recommendation for v1:** Option C (manual author) for the 29 known props. Unblocks
runtime path validation without requiring a new workbench export mode. If the 29 work,
Option A is the automation investment.

### Gap 2: Assimp importer node-name preservation

Does `assimp_importer.cpp` preserve glTF node names when building `TG_MultiShape`?
If it normalizes/drops names, `GetNodeNameId("turret")` will fail even with correct GLB.
Must verify with a test asset before investing in full cook.

**Mitigation:** test single asset (e.g. artilleryturret) with hand-authored GLB before
building cook automation.

### Gap 3: Pivot / bind transform

Current static cook auto-grounds (dy = -minBox.y). For animated props, the rotation
node needs its pivot at the correct point (e.g. turret rotates at its base, not its
centroid). The meshdump pivot field (`[0, 0, 0]`) is always zero — not per-node.

TGL shape has per-node transforms. These must be preserved in the cooked GLB.

### Gap 4: bdAnimData gesture coexistence

4 of the 29 props have both `rotationalNodeId` AND `[Animation:X]` keyframes.
v1 cook ignores keyframes — those props will use legacy TGL for gesture anims.
Not a blocker, but the props will have split state (cooked rotation + legacy gestures).

### Gap 5: bldgRenderShape loading path for animated props

`isStaticEligible()` now blocks static batching for rotational-node props (commit `f191354b`).
But does the runtime load `bldgRenderShape` for animated props at all? Or does it
unconditionally use the legacy TGL shape for animated types?

Must trace `BldgAppearanceType::init()` to confirm `bldgRenderShape` gets loaded
and `GetNodeNameId` will be called on it (not the legacy shape) when override present.

---

## Summary

| Question | Answer |
|---|---|
| Meshdump has node hierarchy? | No — flat, same as static |
| Source GLBs available offline? | No — must author or export separately (Gap 1) |
| Runtime rotation mechanism | `rotationalNodeId` → `SetNodeRotation()`, procedural, no keyframes |
| Node lookup | `GetNodeNameId()` linear string scan on TG_MultiShape node list |
| Cook payload needed | Named-node GLB (base + rotation_node), no animation channels |
| Blocker for v1 cook | Geometry node-split source (Gap 1) |
| Number of props | 29, all single-axis, 6 unique node name values |
| Safe to proceed to Slice 2? | After validating Gap 2 (importer name preservation) with one test asset |
