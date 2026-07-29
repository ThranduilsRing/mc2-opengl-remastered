# DARK-PROP-LIGHTING-6 — STATUS (diagnostic only; NO code shipped)

Branch `claude/nifty-mendeleev` @ `c6889696`. No files edited. Lane released.

## Verdict

The prime suspect handed to this lane (Tree-vs-Bldg light-slot asymmetry) is a
**real code defect but is NOT the cause of the user's dark trucks/crates.**
Those props are `BldgAppearance` — the same class as the buildings beside them.

## 1. The Tree/Bldg asymmetry — CONFIRMED as code fact, at different lines

The status doc's cited lines (`:3638`, `:6706`, `:6719-6720`) do not contain the
claimed code; line drift. Actual receipts, `mclib/bdactor.cpp`:

| | Bldg | Tree |
|---|---|---|
| terrain-light sample lands in | `:3702` `setLightColor(0, lightRGB)` | `:6770` `setLightColor(1, lightRGB)` |
| slot type | `worldLights[0]` = `TG_LIGHT_INFINITE` (`camera.cpp:489`) | `worldLights[1]` = `TG_LIGHT_AMBIENT` (`camera.cpp:516`) |
| sun | left active | `:6783-6784` `getWorldLight(0)->active = false` |
| restored | — | `:6900` `active = true`, `:6907` `setLightColor(1, oldRGB)` |

`:6906`'s own comment ("Set Ambient back to normal color") confirms slot 1 = ambient.
The sun is off across the whole tree-class bake, including
`mc2CacheOrBakeStaticGpuLight` — so tree-class props bake with **zero directional
term**. This fully explains PROP-BASE-LIGHT-DIR-1's finding that `base_light` is
effectively zero and only `ambientTerm` lights these props.

**Why trees were given this treatment:** foliage is crossed alpha-test billboards
with no meaningful normals; a directional term would black out half of every card.
The treatment is correct *for foliage* and wrong for any non-foliage geometry that
lands in the tree class. Any fix must be discriminated on foliage-ness — and
`FOLIAGE-ALPHAFLAG-1` (live, same file) is already landing exactly that
discriminator (`TG_TypeShape::alphaTestOn` via `TG_Shape::GetIsAlphaTest()`).
**Do not build a second one.**

## 2. Why this does not explain the user's props — the class question, CLOSED

`code/dobjtype.h:31-59`; `code/objtype.cpp:368-429`; `code/terrobj.cpp:409-417`,
`:1164-1224`; `code/bldng.cpp:1276-1285`; data at `A:/Games/mc2-srcdata/objects/`.

- `truckprop.fit`, `cargotruckprop.fit`, `fueltruckprop.fit`,
  `ordnancetruckprop.fit`, `armoredcarprop.fit`, `ambulanceprop.fit` →
  `[ObjectClass] ObjectTypeNum=1` = `BUILDING_TYPE` → `BuildingType` → **BldgAppearance**.
- `crates.fit`, `barrels.fit`, `woodencrates.fit` → `ObjectTypeNum=11`
  (`TERRAINOBJECT_TYPE`) but carry a `[TerrainObjectData]` block, so
  `subType == TERROBJ_NONE` → the *else* branch at `terrobj.cpp:1216` →
  **BldgAppearance**. (`art/buildings.csv` labels them "TREE"; that column is
  editor metadata and does not feed `ObjectTypeManager::load`.)
- `TreeAppearance` is reached **only** by fits carrying a `[TreeData]` block — real foliage.

So the user's trucks and crates run the *identical* lighting path as the
correctly-lit buildings. The asymmetry is a separate, still-open defect affecting
actual foliage only.

Caveat: mc2_10's static scenery lives in `mc2_10.pak` (binary PacketFile), not the
`.fit`, and was not decoded — so this is a by-name/by-class proof, not a
per-instance proof for that specific frame.

## 3. How the 3.3x albedo figure arose — partially explained, one hypothesis left

`PROP-LIGHT-CLASS-4-STATUS.md:80` reports truck 41.6 vs building 136.6 in a
"raw texture, no lighting" channel. Checked and **ruled out**:

- Debug view 40 is honest at the shader level: `static_prop.frag:571`
  `dbg = tex_color.rgb`, sampled at `:275` before any lighting. Not contaminated
  by vertex colour.
- sRGB/BC7 decode difference: **ruled out.** Prop albedo arrays are linear BC7
  UNORM — `gos_static_prop_batcher.cpp:3541`, `:4173`
  ("linear BC7 (UNORM): matches GL_RGBA8 baseline; engine has no GL_FRAMEBUFFER_SRGB").
  No gamma curve is inflating the ratio.

Remaining live hypothesis, **not yet tested**: the figures are *region means over
PNG rectangles*, not means over the prop's own pixels. A truck occupies a small
fraction of its capture rect and is surrounded by dark ground/shadow; a building
fills its rect. That alone can manufacture a 3x "albedo" gap from textures the
user correctly reports as normal. Second candidate: atlas `uvScale`/layer
overshoot into the zero-padded region (the hazard `static_prop.frag:253-256`
explicitly guards against). Settle by re-measuring masked to prop fragments, or by
reading the source `.tga` directly — which is what the user did, and which no
prior lane did.

## 4. Live candidate for the actual defect (NOT verified — next lane starts here)

`fogLightSet` is a **one-shot per-actor latch**. Set `false` only in the
constructors (`bdactor.cpp:1517`, `:6033`); the light block at `:3654` runs once
and sets it `true` at `:3699`. So `lightRGB` — which for `BldgAppearance` becomes
the **sun colour** — is sampled from `land->getTerrainLight(position)` exactly
once per actor, at whatever moment that actor first ran `update()`, and never
refreshes.

This is the only mechanism found that can make two actors on the *same* code path
with *normal* textures differ persistently in brightness: whichever props first
updated while the terrain light table was unpopulated / at a different
`nightFactor` keep a stale dark value forever. It predicts exactly the reported
picture — scattered near-black props beside correctly-lit buildings, stable across
frames, unaffected by every lighting gate tried so far (all of which act
downstream of a value that was already latched dark).

**Test before coding:** instrument `lightRGB` at latch time per actor on the
user's mc2_10 compound pose and compare truck vs building actors. If they differ,
that is the bug; the fix is to re-latch (or drop the latch) rather than anything
in the shader.

## 5. What was NOT done

No code change, no build, no deploy, no capture, no tier1 run. **No fix is
claimed and no grading exists.** On a default launch the user still sees the
defect.
