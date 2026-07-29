# PROP-POINT-POOL-1 — STATUS

Lane: PROP-POINT-POOL-1 (ninth lane on the dark-prop defect)
Base: `claude/nifty-mendeleev` @ `4aadd393`
Outcome: **RECON ONLY — no code shipped, and the proposed fix is REFUTED
BEFORE BUILD.** Confirmation (b) failed on measurement. The brief pre-authorized
exactly this stop.

On a DEFAULT launch the user still sees the defect, unchanged. There is no
build, no deploy, no `DEPLOY_FINGERPRINT`, and **no tier1 claim** in this report.

---

## 1. CONFIRMATION (b) — FAILED. The trucks are NOT inside any fixture pool.

### What point/spot lights actually exist on mc2_10

Every object-path `TG_LIGHT_POINT` on this map is the **(E) spotlight-child
light** registered at `mclib/bdactor.cpp:3492`:

```
light->init(TG_LIGHT_POINT);
light->SetaRGB(0xffe8c870);              // warm
light->SetIntensity(0.5f);
light->SetFalloffDistances(20.0f, 80.0f); // far radius = 80 world units
```

Registered lazily, night-only, **one per `SpotLight_` child node of a building
shape**. The remaining point/spot creators are transients — weapon bolts
(`code/weaponbolt.cpp:381`), explosions (`code/carnage.cpp:685`), turret and
mech searchlights (`code/turret.cpp:830`, `mclib/mech3d.cpp:4073`).

**The per-building `pointLight` at `bdactor.cpp:3413` is `TG_LIGHT_TERRAIN`, not
POINT.** The object lighting kernel ignores `TG_LIGHT_TERRAIN` by spec. That is
precisely the terrain-vs-prop asymmetry the user is looking at: the ground under
the truck is lit by a light class the truck's shader is required to skip.

### Measurement

Run: `scripts/run_visual_capture.py --mission mc2_10 --bookmarks
tests/visual/bookmarks/mc2_10_building_faces.json`, exe
`releases/mc2-win64-v0.5.1/mc2.exe`, `MC2_DIAG_VERBOSE=1 MC2_SPOT_DIAG=1`.
Probe engaged (1406 `[SPOT_DIAG v1]` lines; empty-result trap avoided).
Harvested 83 distinct light slots with world positions from
`event=overwrite_first_seen` (71 POINT + 12 SPOT); 196 `our_slot_transition`
lines confirm the (E) building-owned slots were live. Distances below are to the
**nearer of POINT or SPOT** — restricting to POINT alone gives the same figures
to 0.1 u.

Completeness caveat, stated honestly: the 83 slots are those the camera-cull
probe ever saw during a 5-pose sweep, not a proof of the map's full light set.
Two of the five poses sit directly on the user's compound, so the buildings
there certainly ran their lazy night registration; a light that never registered
anywhere in the sweep could still exist elsewhere. This does not move the
compound verdict.

Object positions: mission object list, `mc2_10.pak` packet 1 —
**record stride is 40 bytes, not the 20 asserted in the
`mc2_10_building_faces.json` bookmark note** (`4 + 2629*40 = 105164` = exact file
size). 2629 objects; x/y ranges ±7.4k, cluster at (-2371, 2890) reproduces.

| Measurement | Result |
|---|---|
| POINT far radius (building spotlight nodes) | **80 u** |
| Nearest POINT light to any object in the user's compound | **169 u** (2.1× the radius) |
| Nearest POINT light to the compound centre | 415 u |
| Nearest POINT light to the settlement cluster | 1103 u |
| Map-wide objects within 80 u of any POINT/SPOT light | **50 / 2629 = 1.9 %** |

Those 50 are essentially the buildings that own the spotlight nodes themselves.

**Verdict: no fixture light reaches the user's parked trucks and crates, by a
factor of two on the closest object and by an order of magnitude on the
cluster.** Adding point/spot pools to `static_prop.vert` would change nothing at
the poses the user complained about, while paying a per-vertex light loop on
every prop for a 1.9 % hit rate.

The honest answer for the reported frame is the one the brief named in advance:
**stock-correct — the ambient is simply low for props that carry no emissive
vertex tags, and nothing in the authored scene is meant to light them.** That is
a tuning question (raise night ambient / add a prop fill term), not a bug, and
it must be a user call, not an agent's.

---

## 2. NEW STRUCTURAL FINDING — the object light-position decode is WRONG

Found while designing the (now-cancelled) fix. Independent of the defect, and it
would have silently sunk this lane.

`shaders/include/lighting.hglsl:205-208` documents and uses:

```
vec3 light_pos = ld.light_to_world[i][3].xyz;   // "column 3 = translation"
```

Same expression in `shaders/vulkan/mech_tier1.vert:112,118,124`. **It is always
`(0,0,0)`.** Chain, from source:

1. `Stuff::AffineMatrix4D` stores `Scalar entries[12]`
   (`mclib/stuff/affinematrix.hpp:44-45`), indexed
   `entries[(column<<2)+row]` (`affinematrix.hpp:141`) — i.e. **3 columns × 4
   rows, column-major**. `BuildIdentity` setting `[0],[5],[10]` confirms it;
   `BuildTranslation` writes `(W_Axis, X/Y/Z)` = **`entries[3], [7], [11]`**.
2. `GatherLightsParameters` (`mclib/txmmgr.cpp:2373-2377`) does
   `memcpy(lightToWorld[n], (const float*)light2world, 12*sizeof(float))` then
   **explicitly sets `[12]=[13]=[14]=0, [15]=1`**.
3. `TG_HWLightsData` is uploaded to the SSBO by straight `memcpy` — no
   transpose (`txmmgr.cpp:1919, 2650`).

So under the GLSL column-major read, `light_to_world[i][3].xyz` is the zeroed
column, and the real world position sits at
**`vec3(m[0].w, m[1].w, m[2].w)`**.

Consequence: **every point/spot light on the object path is evaluated as if it
sat at the world origin** in both the GL kernel and the Vulkan mech port. The
`MC2_LIGHT_CLAMP_FIXTURE` synthetic lights in the same function use identity
`lightToWorld` with `farDistance = 1000.0` "so the synthetic lights actually
reach the object" — consistent with an author who had already observed this.

Not fixed here (out of lane, and it is a shared kernel touching mech + VFX +
Vulkan). **Any future lane that adds pools to `static_prop.vert` must use
`m[i][0].w/[1].w/[2].w`, or it will light props from the map corner.** Worth its
own slice with its own parity gate.

---

## 3. CONFIRMATION (a) — NOT CLOSED, but sharpened (and the prior fit is refuted)

`MC2_GPU_CULL_STATIC_LIGHT_ZERO_PROBE` produced **zero `[BAKEORD v1]` lines**:
that probe is on nifty HEAD, not in the shipped `v0.5.1` exe. Closing (a)
empirically needs a build; I did not do one, because (b) had already cancelled
the slice.

By source, however, the prior lane's proposed provenance for `63` is **wrong**:

- `mc2_10.fit` authors `LightDirPitch = 180.0`, `DayLightPitch = 180.0`.
- `camera.cpp:1868-1872`: `fAmbientRed = (dayAmbientRed - nightAmbientRed) *
  sin(lightPitch) + nightAmbientRed`. At pitch 180, `sin = 0` → ambient = **32**
  (0.1255), not 63 (0.2471).

So `0.2471` cannot come from the authored pitch. It requires `sin(pitch) ≈
0.194`, i.e. `lightPitch ≈ 168.8°` — an **animated** pitch, moved off its
authored value by the running day/night cycle. That is a new detail, not a
receipt. It does not disturb the §1 polarity ruling of PROP-AMBIENT-POLARITY-1,
which rests on the exact `192/255 = 0.752941` match and the load-order
mechanism; only the explanation of `63` is still open.

---

## 4. WHAT THIS CLOSES

- **`PROP-POINT-POOL-1` as specified is DEAD.** Do not implement point/spot
  pools in `static_prop.vert` to fix the dark props. The pools do not reach the
  props (169 u vs an 80 u radius; 1.9 % map-wide hit rate).
- `MC2_STATIC_PROP_LIGHT_POOLS` was **never created** — no registry, docs,
  allowlist or smoke-passthrough entries were added, so there is no dead gate to
  clean up.
- The remaining candidate causes for the user's frame are **tuning**, not a
  missing path: night ambient for untagged props, or a deliberate prop fill
  term. Both change the whole scene and need the user in the loop —
  `mc2_05`'s night lighting is user-approved and any global ambient move puts it
  at risk.

## 5. ARTEFACTS

- `scratch/ppp1/capA/` — capture set + `mc2_10_capture.log` (1406 SPOT_DIAG lines)
- `scratch/ppp1/lights.txt` — 83 light slots, type + world position
- `scratch/ppp1/objs.txt` — 2629 mission objects, type + world position
