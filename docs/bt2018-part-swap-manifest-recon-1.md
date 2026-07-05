# BT2018-PART-SWAP-MANIFEST-RECON-1 — manifest-driven mech part swapping

**Status:** RECON / DESIGN ONLY (no implementation). Read-only sweep.
**Worktree:** `A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/`
**Goal:** Compose one imported mech from limbs of multiple BattleTech-2018 mechs, driven
by a manifest, by substituting donor meshes into the host part loop BEFORE the
merge-collapse at `PopulateMergedSkinnedShape`.

---

## 0. Confirmed facts (harness `inspect`, two mechs)

Harness: `build64-harness/RelWithDebInfo/mech_import_harness.exe inspect <glb>`

### Part (mesh) naming — chassis prefix + region tokens

| Atlas (`atlas_fbx2gltf.glb`, 56 meshes) | Marauder (`marauder_fbx2gltf.glb`, 69 meshes) |
|---|---|
| `atlas_left_arm_shoulder` (m12) | `mad_left_arm_clavicle` (m5) |
| `atlas_left_arm_upperarm` (m14) | `mad_left_arm_upperarm` (m7) |
| `atlas_left_arm_forearm` (m11) | `mad_left_arm_forearm` (m6) |
| `atlas_left_arm_elbow` (m10) | *(no elbow part)* |
| `atlas_left_arm_shoulderpad` (m13) | `mad_left_arm_clavicle` (m5) |
| `atlas_left_leg_thigh/calf/foot/toe` | `mad_left_leg_thigh/calf/foot` + `talon1/2`, `toe_index1/2`, `toe_pinky1/2`, `hip` |
| `atlas_centre_torso` (m53) | `mad_centre_torso` (m65) |
| `atlas_centre_torso_pelvis` (m54) | `mad_centre_torso_pelvis` (m66) |
| `atlas_left_torso` / `atlas_right_torso` | `mad_left_torso` / `mad_right_torso` |
| `atlas_head` (m4), `atlas_head_eyes` (m5) | *(marauder has no head meshes — cockpit is in centre_torso)* |

- Mesh-name structure is `<chassisPrefix>_<region>_<subpart>[_dmg|_explode]`.
  Chassis prefix differs (`atlas`, `mad`); region/subpart tokens are stable across rigs
  for the *humanoid* regions (`left_arm`, `right_arm`, `*_torso`).
- `_dmg` (damage variant) and `_explode` (debris) meshes are dropped at import via
  `skelMeshDropped()` (`assimp_importer.cpp:181` → `mech_texname::isDroppedMeshName`).
  Only intact `bones=1` meshes survive into the merge.
- `uixMdlIndc_*Blip` meshes are `bones=0` → skipped in the skinned path
  (`assimp_importer.cpp:587`).

### Skeleton — major-limb joint NAMES are shared

Atlas: 17 bones. Marauder: 28 bones. **Common bone set (one-to-one identical names):**

```
j_Root, j_Pelvis, j_Spine1, j_Spine2, j_Neck(atlas)/—, j_Head(atlas only),
j_LClavicle, j_LUpperArm, j_LForearm,           ← LEFT ARM chain (identical both)
j_RClavicle, j_RUpperArm, j_RForearm,           ← RIGHT ARM chain (identical both)
j_LHip, j_LThigh, j_LCalf, j_LFoot,             ← LEFT LEG chain down to foot
j_RHip, j_RThigh, j_RCalf, j_RFoot              ← RIGHT LEG chain down to foot
```

**Divergence (leaf joints below the foot / head):**
- Atlas legs terminate at `j_LToe0`/`j_RToe0` (humanoid toe).
- Marauder (chicken/reverse leg) has NO `j_Toe0`; instead `j_LFoot` parents
  `j_LIndex1→2`, `j_LPinky1→2`, `j_LTalon1→2` (digitigrade talon claws). Same for R.
- Atlas has `j_Head`/`j_Neck`; marauder has neither (cockpit baked into torso).

Parent chains for the shared bones are byte-identical
(`j_LForearm→j_LUpperArm→j_LClavicle→j_Spine2`, `j_LCalf→j_LThigh→j_LHip→j_Pelvis`).

**Conclusion:** the **arm chain** (`j_?Clavicle/UpperArm/Forearm`) is fully shared and
swappable between humanoid and reverse-joint chassis. The **leg chain** is shared *only
down to `j_?Foot`*; the foot-and-below differs structurally (toe vs talon) — see §3.4.

---

## 1. Manifest schema

A region maps to (a) a set of host-skeleton attach bones, (b) the donor GLB, and
(c) a mesh-name glob that selects the donor's intact meshes for that region. The
manifest replaces the host's meshes for that region with the donor's.

### Regions (host-skeleton-relative)

| Region id | Attach bone(s) (host skeleton) | Mesh-name token matched |
|---|---|---|
| `left_arm`  | `j_LClavicle, j_LUpperArm, j_LForearm` | `*_left_arm_*` |
| `right_arm` | `j_RClavicle, j_RUpperArm, j_RForearm` | `*_right_arm_*` |
| `left_leg`  | `j_LHip, j_LThigh, j_LCalf, j_LFoot (+ leaf)` | `*_left_leg_*` |
| `right_leg` | `j_RHip, j_RThigh, j_RCalf, j_RFoot (+ leaf)` | `*_right_leg_*` |
| `center_torso` | `j_Pelvis, j_Spine1, j_Spine2` | `*_centre_torso_*` / `*_center_torso_*` |
| `head` | `j_Neck, j_Head` | `*_head*` (humanoid hosts only) |

> Note both `centre`/`center` spellings appear (atlas uses both: `atlas_centre_torso`
> + `atlas_center_torso_pelvis_rear`). Region globs must match both.

### Concrete manifest (JSON example)

```json
{
  "host": "data/tgl/atlas_fbx2gltf.glb",
  "name": "frankenmech_madcat_arms",
  "swaps": [
    {
      "region": "left_arm",
      "donor": "data/tgl/marauder_fbx2gltf.glb",
      "meshGlob": "mad_left_arm_*",
      "attachBones": ["j_LClavicle", "j_LUpperArm", "j_LForearm"]
    },
    {
      "region": "right_arm",
      "donor": "data/tgl/marauder_fbx2gltf.glb",
      "meshGlob": "mad_right_arm_*"
    }
  ]
}
```

- `region`: one of the fixed ids above; defines the host attach-bone set + the host
  mesh token it *removes*.
- `donor`: GLB whose skeleton SHARES the named attach bones with the host (validated).
- `meshGlob`: selects donor intact meshes (skip `_dmg`/`_explode` automatically; the
  glob is matched against the donor mesh name after `skelMeshDropped` filtering).
- `attachBones` (optional): explicit donor-bone→host-bone identity map. Default =
  identity by name (donor mesh's `mBones[0]->mName` must exist in host `nameIdx`).

**Region→mesh-set rule:** a region owns every host mesh whose name contains the
region token. Swap = drop all host meshes for that token, then ingest all donor
meshes matching `meshGlob` and re-tag their single bone to the host bone of the same
name.

---

## 2. Multi-scene load + combined part list

Today `ImportGeometryFromFile` (`assimp_importer.cpp:1165`) reads ONE scene
(`imp.ReadFile`, :1172) and builds ONE `SkelBake` from it
(`BuildSkeleton`, :1208). The part loop in `PopulateMergedSkinnedShape` (:603)
walks `scene->mMeshes`.

**Design for multi-scene:**

1. Read the HOST scene as today → `BuildSkeleton(host)` → `bake.names/rest/nameIdx`.
   The host skeleton is the AUTHORITY: all runtime bone matrices come from host
   `rest[]` (and at runtime, host clip globals). Donors contribute geometry only.
2. For each distinct donor GLB in the manifest, `ReadFile` it into its own
   `aiScene*` (own `Assimp::Importer`, kept alive for the whole merge — Assimp owns
   the mesh/bone memory). Cache by path so a donor used for both arms loads once.
3. Build a **combined part list** = list of `(const aiScene* srcScene, unsigned
   meshIndex, hostBoneIndex)` records, replacing `parts` (the `std::vector<unsigned>`
   at :581):
   - For each host mesh: if its region token is owned by a manifest swap → SKIP
     (host part dropped). Else keep `(hostScene, m, hostBone)`.
   - For each swap, for each donor mesh matching `meshGlob` (and not `skelMeshDropped`,
     `mNumBones>0`): resolve donor bone name → host `nameIdx`. If found, append
     `(donorScene, m, hostBoneIndex)`. If NOT found (leaf-joint gap, §3.4) → reject
     that mesh + log.
4. `PopulateMergedSkinnedShape` iterates the combined list instead of `scene->mMeshes`;
   the per-vertex bake uses `rm = host rest[hostBoneIndex]` and `off =
   donorMesh->mBones[0]->mOffsetMatrix` (the donor's own bind offset). `totalV/totalT`
   sum over the combined list, not one scene.

`ImportPartRec.meshIndex` (`:129`) must become `(scene*, meshIndex)` for the runtime
re-bake path to find the right donor mesh; or capture donor verts at bake time and
store only `boneIndex/off/vOff/vCount` (boneIndex already indexes host `names`, so
the per-frame palette path is unchanged — this is the cleaner option).

---

## 3. Constraint analysis (risk order, highest first)

### 3.1 Bind-offset mismatch (HIGHEST RISK — floating/dislocated limb)

The bake (`assimp_importer.cpp:630`):
```
p = applyRowMajor16(rm, off * me->mVertices[v]) * bake.scale
        rm = HOST rest[bone]   off = mesh's own offsetMatrix (bind, donor authoring)
```
`off` puts donor verts into donor-bone-LOCAL space (cancels the donor's bind world
placement of that bone). `rm` (host rest global) then re-plants them at the HOST
bone's world position. **This composition is the swap mechanism** — IF the donor's
`off` is a true inverse-bind for that bone (cancels donor bind) and the host `rm` is
the host bind global, the limb lands at the host joint correctly REGARDLESS of where
the donor authored it. That is exactly the per-part-offset design already proven for
same-mech import (header `mech_skel_import.h:27-32`).

**Residual risk:** the limb is correctly *positioned/oriented* at the host joint, but
its *proportions* are the donor's (donor bone segment lengths baked into the donor
verts). A marauder forearm welded to an atlas upperarm meets at the host `j_LForearm`
world point — but the marauder forearm geometry extends to the marauder's forearm
length, which need not equal the atlas's. Visible as a length step at the elbow, not a
float. **Acceptable for arms; quantify before shipping** (compare host vs donor
`rest[j_?Forearm]` translation magnitudes).

Hard-float failure only if donor `off` is NOT a clean inverse-bind (e.g. an identity
offset on a stray) — guard: require `meshDonor->mNumBones==1` and bone-name resolves.

### 3.2 Scale proportion (MEDIUM)

`bake.scale` (`:1250-1269`) is computed from the HOST bind extent → target height 50.
Donor verts are multiplied by the SAME host `bake.scale`. A marauder arm authored at
marauder Unity scale gets host-scale applied — but the marauder's native units differ
from the atlas's. The donor `off`-then-`rm` already maps into host bind space (host
units), so the host scale is the correct multiplier ONLY if donor and host author at
the same base unit. BT2018 GLBs appear to share Unity authoring scale (both ~0.15u
tall pre-scale), so this is likely fine, but **must be verified per donor** (measure
donor intact extent vs host). If donors differ, store a per-donor pre-scale.

### 3.3 Texture / atlas mismatch (MEDIUM — DEFERRED, flag only)

Current merge forces ALL tris to `localTextureHandle = 0` (`:657`) and binds ONE
slot-0 atlas = the host body's `chrMatMech_*_base` (`:1308-1316`). A donor arm's UVs
index the DONOR atlas (`chrMatMech_marauder_base`), not the host's. Welding donor geo
but sampling host atlas → garbage texels on the swapped limb.
**DEFERRED per scope.** Flag: part-swap needs a multi-slot atlas path (donor atlas as
slot 1, donor tris `localTextureHandle=1`) OR an atlas-merge bake. This is the single
biggest content-correctness gap and is explicitly out of scope for this recon. The
single-slot assumption at `:1304-1307` ("Weapons/second-atlas mechs are a later
slice") is the same blocker.

### 3.4 Leaf-joint name gaps / reverse-leg interaction (MEDIUM — bounds the unit)

Donor mesh whose `mBones[0]` name is absent from the host skeleton CANNOT be placed
(no host `rest[]` entry → step 3 rejects it).
- **Arms:** zero gap (clavicle/upperarm/forearm shared) → arms swap cleanly in BOTH
  directions (humanoid↔reverse chassis).
- **Legs:** thigh/calf/foot bones are shared, but a marauder leg's talon/toe meshes
  bind to `j_LTalon1/2`, `j_LIndex1/2`, `j_LPinky1/2` — ABSENT from the atlas host →
  those donor meshes are rejected, producing a marauder leg with NO foot claws on an
  atlas. Reverse: an atlas leg's `*_toe` binds `j_LToe0` — absent from a marauder host
  → toeless. So **whole-leg swap across the humanoid/reverse boundary is LOSSY** (loses
  the below-foot geometry). Leg swap is clean only between SAME-class chassis
  (humanoid↔humanoid or reverse↔reverse) where the leaf bones match.
- This is the same humanoid-vs-reverse classification the project already tracks for
  mechs (cf. MECH-FIREANT-ORIENTATION recon).

Mitigation option (future): extend the host skeleton with the donor's missing leaf
bones at import (append to `bake.names/rest` from the donor's node defaults). Out of
scope here — flag only.

### Constraint summary table

| Risk | Severity | Symptom | Swappable today? |
|---|---|---|---|
| Bind-offset mismatch | High (mechanism) | limb floats/dislocates | YES if donor bone name∈host & `off` is clean inverse-bind |
| Proportion (segment length) | Med | length step at joint seam | YES (cosmetic), quantify |
| Scale (unit) | Med | limb too big/small | YES if donors share authoring unit (verify) |
| Texture/atlas | Med | wrong texels on swapped limb | NO — needs multi-slot/atlas-merge (DEFERRED) |
| Leaf-joint gap (reverse leg) | Med | missing foot/toe/talon geo | Arms YES always; legs only same-class |

---

## 4. Injection-point pseudocode (`assimp_importer.cpp` ~603–677 + ~581)

Inject in `PopulateMergedSkinnedShape` by replacing the single-scene `parts` list
(`:581-589`) with a combined cross-scene list, and the per-part bone lookup
(`:604-610`) with a host-bone resolution that accepts a donor scene.

```cpp
// --- replaces :581-589 (single-scene part gather) ---
struct CombinedPart { const aiScene* scene; unsigned mesh; int hostBone; };
std::vector<CombinedPart> parts;
for (each host mesh m in hostScene) {
    if (mesh dropped / bones==0) continue;
    const char* token = regionTokenOf(name);              // left_arm, etc.
    if (manifest.swapOwnsRegion(token)) continue;         // host part removed
    int hb = bake.nameIdx[ hostMesh->mBones[0]->mName ];  // identity (host on host)
    parts.push_back({hostScene, m, hb});
}
for (each swap S in manifest) {
    for (each donor mesh dm in S.donorScene matching S.meshGlob, intact, bones>0) {
        auto it = bake.nameIdx.find(dm->mBones[0]->mName.C_Str());
        if (it == bake.nameIdx.end()) { log_reject(dm,"bone not in host"); continue; } // §3.4
        parts.push_back({S.donorScene, donorMeshIndex, it->second});
    }
}
// totalV/totalT now summed over `parts` (cross-scene).

// --- replaces :603-610 (per-part bake) ---
for (const CombinedPart& cp : parts) {
    const aiMesh* me = cp.scene->mMeshes[cp.mesh];
    const float* rm  = bake.rest[cp.hostBone].m;          // HOST rest global (authority)
    aiMatrix4x4 off  = me->mBones[0]->mOffsetMatrix;      // DONOR's own bind inverse
    // identical bake math from here (:627-672), unchanged:
    //   p = applyRowMajor16(rm, off * me->mVertices[v]) * bake.scale
    // ImportPartRec.boneIndex = cp.hostBone (host palette index) -> runtime path unchanged
}
```

The merge-collapse (`shape->InitFromImportedMesh`, `:677`) and everything downstream
(GPU recipe, batcher, runtime palette via host `names`) is UNTOUCHED — the swap lives
entirely in the part-gather + per-part bone resolution, exactly as the prompt's
injection thesis states.

---

## 5. Swappable-unit verdict

**Minimal robust swappable unit = a whole ARM (clavicle + upperarm + forearm chain)
between any two chassis** — the arm bone chain (`j_?Clavicle/UpperArm/Forearm`) is
name-identical and parent-identical across humanoid (atlas) and reverse-joint
(marauder) rigs, so the bind-offset composition (§3.1) lands it correctly with NO
leaf-joint gap (§3.4). Arms are the clean, direction-symmetric unit.

**Whole LEG is swappable only within the same leg class** (humanoid↔humanoid or
reverse↔reverse); cross-class leg swap loses the below-foot geometry (toe vs talon
leaf-bone gap).

**Torso/head:** torso shares `j_Pelvis/Spine1/Spine2` (swappable), but head exists
only on humanoid hosts.

**Hard blocker for ALL swaps to look correct: texture/atlas (§3.3, DEFERRED).** Until
a multi-slot or atlas-merge path exists, swapped limbs sample the host atlas and will
be mis-textured. Geometry composition (this recon's scope) is sound and minimally
invasive today; visual correctness is gated on the deferred atlas slice.

**Recommended first slice:** single arm swap, same-or-cross class, accept temporary
mis-texturing (or pick a donor whose base atlas is visually close), to prove the
bind-offset composition end-to-end before building the atlas path.

---

## Quoted evidence index

- Part loop / merge collapse: `mclib/assimp_importer.cpp:580-679`
  (gather :581-589, per-part bone :603-610, bake formula :630, ImportPartRec push
  :616-623, `InitFromImportedMesh` collapse :677).
- `ImportPartRec` struct: `assimp_importer.cpp:128-134`.
- Single-scene `ReadFile` + `BuildSkeleton`: `assimp_importer.cpp:1172, 1208`.
- Single-slot atlas assumption: `assimp_importer.cpp:1304-1316` (forced `localTextureHandle=0` at :657).
- Per-part-offset rig rule: `mclib/mech_skel_import.h:27-32`; `mech_skel_import.cpp:174-185`.
- `BuildSkeleton` ordered bone de-dup + parent resolution: `mech_skel_import.cpp:113-146`.
- Harness mesh/bone names: atlas (17 bones, `j_Head` root, `j_?Toe0` leaf;
  `atlas_left_arm_forearm` etc.); marauder (28 bones, `j_Pelvis` root,
  `j_?Talon/Index/Pinky` leaf; `mad_left_arm_forearm` etc.).
