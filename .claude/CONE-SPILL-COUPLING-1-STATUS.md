# CONE-SPILL-COUPLING-1 status — 2026-07-25

Lane `CONE-SPILL-COUPLING-1`, worktree `A:/Games/mc2-main`, branch
`claude/nifty-mendeleev`, HEAD at start `11ec0d5a`.
Claimed: `GameOS/gameos/gos_terrain_lighting.cpp`, this doc.
Baseline per user: **variant B / STRONGER** (LIGHT-REALISM-1).

**STATUS: RECON COMPLETE — NO CODE CHANGE MADE. Re-scoping decision required.**
The authorized file cannot host the requested fix. Evidence below.

---

## The user's two requirements

1. Cone volumes **materially more transparent** — ground texture must read
   through them clearly.
2. **Geometric coupling**: where the cone's volume intersects the ground is
   where the pool must be centred, brightest at the intersection centre,
   radius from the cone's spread at that distance; no phantom pool when the
   fixture is aimed at a wall or off-map.

## Headline finding: the coupling already exists, and is wrong in four specific ways

Requirement 2 is **not** unimplemented. `LIGHT-CONE-MODELS-1` already built it,
default-ON under `MC2_LIGHT_CONE_NODES`. The live code is
`mclib/bdactor.cpp:3292-3312` (buildings — the guard towers the user is
grading), mirrored at `gvactor.cpp:2713` and `mech3d.cpp:4225`:

```cpp
Stuff::Vector3D childPos = getNodeIdPosition(spotlightNodeIds_[k]);
if (mc2_cone_glow::nodes_enabled()) {
    if (mc2_cone_glow::GetNodeAim(bldgShape, spotlightNodeIds_[k], aimW, coneLen)) {
        float spill = coneLen * mc2_cone_glow::spill_scale();
        if (spill > 200.0f) spill = 200.0f;
        childPos.x += aimW.x * spill;
        childPos.y += aimW.y * spill;
        childPos.z += aimW.z * spill;
        if (land) childPos.z = land->getTerrainElevation(childPos);
    }
}
spotlightLights_[k]->SetPosition(&childPos);
```

Four defects, each mapping onto something the user can see:

| # | Defect | What the user sees |
|---|---|---|
| 1 | **Vertical drop, not a ray/terrain intersection.** `childPos = origin + aim*spill`, then `z` is *snapped* to `getTerrainElevation`. That is the far-end point projected straight DOWN, not where the beam actually meets ground. For a shallow-angled tower searchlight over rising ground the beam meets terrain well BEFORE the far end. | Pool sits beyond / off to the side of where the cone visibly lands — "cone points one way, pool sits elsewhere". |
| 2 | **Arbitrary `spill > 200.0f` clamp.** A long beam is truncated at 200 world units regardless of geometry. | Long beams' pools land short of the visible cone tip. |
| 3 | **Radius never derived from cone spread.** `SetFalloffDistances` is called ONCE at light creation from the building's FIT (`terrainLightInnerRadius/OuterRadius`, `bdactor.cpp:3167`) and never revisited. Nothing anywhere ties pool radius to the cone's spread at the intersection distance. | Pool size unrelated to beam width — the two read as separate effects. |
| 4 | **No miss rejection.** If the ray never meets ground within the beam (aimed at a wall, over a cliff, off-map), the code still plants a pool at the clamped far end. | Phantom pool on distant ground — explicitly called out in the brief. |

## Why this CANNOT be done in `gos_terrain_lighting.cpp` (my authorized file)

My earlier recon suggested `TG_Light` carried the aim, and I flagged that it
needed confirming before writing math. **Confirmed: it does not.** For the
building/guard-tower fixtures being graded (`mclib/tgl.h:199`):

- `lightType` is `TG_LIGHT_TERRAIN`, not `TG_LIGHT_SPOT`.
- `direction` is documented "light spot center for POINT and SPOT" but is
  assigned the fixture's **own position**, coordinate-swizzled
  (`bdactor.cpp:3174`, `pointLight->direction = ourPosition`) — degenerate,
  carries no aim.
- `spotDir` is **never written** on this path → stays `(0,0,0)` from `init()`.
- `maxSpotLength` is **never written** → stays `0.0f`.

Only turrets (`code/turret.cpp:908-914`) populate `spotDir`/`maxSpotLength`,
and there `direction` is the aim target while `spotDir` is the turret's own
position — the reverse of what the names suggest.

By the time a light reaches `gos_terrain_lighting.cpp`'s packing loop
(~line 1019) it is a bare `TG_Light`: a single already-grounded `position`
plus `closeDistance`/`farDistance`. **The cone axis, the node origin, the cone
length and the spread have all been destroyed upstream.** The producer file I
was authorized to edit is downstream of the information the coupling needs.
`LIGHT-REALISM-1` could reshape the pool's *falloff* there precisely because
falloff needs no geometry — *centring* does.

## Where the fix actually belongs

- **Defects 1, 2, 4** → `mclib/bdactor.cpp` (+ `gvactor.cpp`, `mech3d.cpp` for
  movers). Replace the drop-and-snap with a real march of the beam ray against
  terrain elevation, drop the 200-unit clamp, and return "no intersection" so
  the caller can deactivate the pool instead of planting it.
- **Defect 3** → needs a cone **spread/half-angle**, which no public API
  exposes. `mc2_cone_glow::GetNodeAim` (`cone_glow.cpp:266`) returns aim +
  length only; the base radius exists inside `localConeAxis`/the type geometry
  but is not surfaced. Requires a new accessor in `mclib/cone_glow.cpp`.

### Ownership at time of writing

| File | Fresh-lane claim | Verdict |
|---|---|---|
| `GameOS/gameos/gos_terrain_lighting.cpp` | mine | authorized, **but wrong seam** |
| `mclib/bdactor.cpp` | none (all claimants stale ≥1.8 d) | **free — the real seam** |
| `mclib/cone_glow.*` | GHOST-CONE-VERIFY-4 (live) | contended |
| `mclib/gvactor.cpp` | GHOST-CONE-VERIFY-4 + VEHICLE-SELFLIT-4 (live) | contended |
| `mclib/mech3d.cpp` | GHOST-CONE-VERIFY-4 (live) | contended |

`bdactor.cpp` is the static-building path and covers the guard towers in the
user's grading pose. It is conflict-free but **outside my current grant**, so
I did not touch it. Doing buildings alone would deliver requirement 2 for the
graded fixtures without touching any contended file.

---

## HANDOFF: cone opacity

**For the `mclib/cone_glow.cpp` owner (GHOST-CONE-VERIFY-4) or a follow-up
lane. I did not apply this — the file is contended.**

- **File / line:** `mclib/cone_glow.cpp:222`
- **Symbol:** `MC2_LIGHT_REALISM_CONE_OPACITY` default
- **Current source default:** `0.55f`
- **Proposed:** `0.15f`

**Correction to the brief:** `0.55` is variant **A**/source default. Variant
**B** — the user's chosen baseline — is the env override `CONE_OPACITY=0.40`
(LIGHT-REALISM-1 status, "Variants shipped for taste grading"). The proposal
is therefore relative to **0.40**, and `0.15` is a ~2.7x reduction from B
(~3.7x from the source default).

**Rationale, and a caveat that matters:** the cone batch is
`gos_Alpha_OneOne` — **additive** (`dst += src`), not alpha-blended
(`cone_glow.cpp:782`). The wedge reads as a solid painted triangle because
accumulated additive contribution **saturates toward white** where many cone
triangles overlap along the view ray, not because an alpha value is high.
Effective peak is `MC2_LIGHT_CONE_GLOW_ALPHA` (default 40/255) × `CONE_OPACITY`
× nightFactor, so B is already only ~16/255 per layer — yet it still blows out
where layers stack. Lowering the scalar is the correct first lever and should
be done, but if the wedges still read solid at `0.15`, the residual cure is
**clamping accumulated contribution per fragment**, not pushing the scalar
further toward zero (which will kill the beam near the fixture before it fixes
the saturated core). Grade by eye, not by number.

---

## Deploy / verification

**Nothing was built, deployed, or captured.** No code changed, so there is
nothing to attribute and no crops to show. Producing evidence for an unchanged
binary would be hollow, so I did not run tier1 or the night captures.

Standing authorization for when work resumes (from the coordinator): evidence
deploy may proceed **`-dirty`-stamped**, because sibling lane
RUNWAY-APRON-TONE-2 holds uncommitted `GameOS/gameos/gos_terrain_indirect.cpp`
in this shared worktree. Do not commit, stash or touch that file. State the
caveat in the REVIEW section, name your own commit sha explicitly, and md5 the
deployed exe/shaders against the build so attribution holds **by content**
even though the fingerprint gate cannot pass. Skip tier1 if it cannot be
attributed and say so rather than reporting a hollow pass.

## Guard conditions cleared

- `preflight` `safe_to_touch=false` (4 untracked scratch dirs) — **ACKed** by
  coordinator; WORKTREE-HYGIENE-1 established `dirty` is computed `-uno`, so
  untracked files never affected the build fingerprint.
- Lane registered, no file conflicts on the claimed set.

## Open decision

Re-point the grant from `gos_terrain_lighting.cpp` to `mclib/bdactor.cpp`
(defects 1, 2, 4 — buildings only, no contended files), and route defect 3's
spread accessor to the `cone_glow.cpp` owner alongside the opacity handoff.
