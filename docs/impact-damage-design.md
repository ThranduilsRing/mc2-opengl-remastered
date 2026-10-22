# Weapon impact damage visuals

Owner intent (2026-10-22): a gun busts a hole through sheet metal, a laser burns it, a missile breaks through and explodes.
The answer is a **portable impact library** (weapon class x surface material) applied as **box decals with surface
deformation** on any surface, not per-building damage art. Tier 1 ships default-ON on branch `claude/impact-decals`.

## 1. Event source (presentation only)

- The simulation pushes an `impact::Event` where it already resolves a shot or an explosion:
  - `WeaponBolt::update` (projectile arrival and beam end), hit and land-miss branches (`code/weaponbolt_update.cpp`).
    Water misses emit nothing.
  - `GameObjectManager::createExplosion` for mines, vehicle/mech/turret/building deaths, DFA and airstrikes
    (`code/objmgr_misc.cpp`). Bolt explosions are not re-emitted.
- Payload: sim tick (`turn`), scenario time, hit point, shot direction, target watch id, weapon class
  (`classifyWeapon`: component form + beam/area/artillery/inferno flags), damage or blast radius.
- One-way: a bounded queue (`impact::EventQueue`, 48, oldest dropped) that the simulation never reads. No RNG is drawn;
  variation comes from a stateless hash of the event (`presentationSeed`). Saves, replays and `MC2_DETERMINISTIC_RNG` are
  untouched. The 30 Hz fixed tick (claude/gamelogic-R1) only changes how many ticks feed a frame; the queue absorbs it.
- The presentation side (`impact_presentation::frame`, once per rendered frame) resolves the target read-only through the
  object manager: mover -> `Armor`, building/prop -> sheet metal or concrete, otherwise terrain type -> soil or concrete.

## 2. Technique

- **Box decals in the opaque tail.** One Vulkan pass right after the terrain overlay/decal batch and before water,
  particles and post (`mc2_vk_impact_decals_run`, `GameOS/gameos/vulkan_impact_decals.cpp`). Each decal is an oriented
  box drawn instanced (36 vertices); the vertex shader drops camera-facing faces, so every covered pixel shades once,
  including when the camera is inside the box. The fragment reconstructs the surface from the shared depth (reverse-Z),
  rejects pixels outside the box, on a surface class the decal does not affect (scene-normal alpha: terrain 1.0, props
  0.19-0.25, units 0), or facing away from the shot, and fades along the projection axis.
- **Material inputs before lighting.** MC2 is forward-lit, so the decal edits the lit opaque colour by ratios that are
  exactly what an albedo/normal change would have produced in the diffuse term:
  - albedo: per-channel multiply (soot, char tint, bare-metal lips up to 2x, black holes);
  - deformation: the geometric normal (from depth neighbours) is tilted by the atlas tangent normal and the sun term is
    relit by `lit(tilted) / lit(flat)`, so craters, petals, groove lips and chip bowls catch the sun;
  - emissive: an additive pass for hot decals only.
  Screen shadow, fog and tone mapping later in the frame apply to the decal like to any surface.
- **Holes in sheet metal.** Tier 1 renders the puncture as a black core with a raised, bright torn lip (depth read by the
  relight). A true see-through cut needs what is behind the wall, which a forward renderer has not drawn: it moves to tier 2
  (kit-generated damaged sections, section 6).
- **Atlas.** `RenderCore/ImpactDecalAtlas.h` paints eight 256x256 layers procedurally (clean-room, deterministic, no
  asset dependency, so mods cannot break it): two RGBA8 2D-array stacks with mips, about 2.8 MB, owned by the texture
  residency budget. Surface: albedo ratio, emissive mask, char, coverage. Normal: tangent xy, height, roughness (reserved).

## 3. Impact library (`impact::recipeFor`)

| weapon \\ material | sheet metal / armour | concrete / ferrocrete | soil (terrain) |
|---|---|---|---|
| ballistic | puncture cluster: black holes, bent petals, bare-metal ring, 0.25 s hot lip | spall chip: lighter aggregate, dark pits, radial cracks | chip crater (larger) |
| laser | melt groove with molten lips and heat band, glow cools over 1.6 s (white-orange -> dull red) | glassed streak with cross cracks, 1.0 s glow | glassed streak along the shot heading |
| PPC | discharge starburst, blue-white burn lines, 0.6 s | same | same |
| missile | torn breach: jagged hole, petals bent outward, heavy soot, brief edge flash | crater bowl, raised rim, ejecta rays, char | larger crater |
| splash (deaths, mines, inferno) | soft soot blot, no relief | same | same |
| artillery / airstrike | large crater | same | same |

A building hit paints the building with its own response and the ground at its foot with the weapon's ground response.
Sparks and smoke stay the existing per-weapon gosFX hit effects; the material-aware spark choice is a later table entry
keyed on the same (weapon, material) pair (`fx_def_registry` override), not a new system.

## 4. Lifetime, fade, merge, budget

- Lifetimes (scenario seconds, so pause freezes them): ballistic 45, laser 50, PPC 60, missile 90, splash 75,
  artillery 120, mover scars 25. Full strength for 70% of the life, then a smooth fall to zero.
- Merge: a new impact within half a radius of a live decal of the same class on the same anchor refreshes it (glow
  re-ignites, intensity +25%, size x1.12 up to 1.6x) instead of stacking. Sustained fire costs nothing extra.
- Budget: quality 2 = 256 decals, 1 = 96 (no unit scars), 0 = off. Full pool: the decal with the least remaining life is
  evicted. At most 32 spawns per frame. Render option `ImpactDecals` in `options.cfg` (default 2); the potato/base-game
  preset sets 0. No new environment gate.
- Cost is bounded by covered screen area x 7 texture fetches; an empty frame records nothing.

## 5. Per-object vs world decals

- **World**: on terrain, projected straight down, in-plane axis along the shot heading.
- **Static** (buildings, props): stored in the anchor frame; the box reaches back toward the shooter (the sim's hit point
  can lie inside the hull) and only the facing wall survives the facing test. Decals die when the building is destroyed,
  so the destroyed-state art is never painted with the intact building's scars.
- **Mover** (mechs, vehicles): stored relative to the root position and yaw and moved every frame; affect = units only,
  so the ground under a walking mech is never painted and terrain scars never paint a mech walking through them.
  Limb animation is not followed in tier 1 (scars are small and short-lived).

## 6. Composition with shadows and lighting

The decal modifies material response before the frame composition; it does not own lighting. When the lighting lane
(claude/lighting-L0L1) exposes a material G-buffer (albedo/roughness/emissive), the same fragment writes those inputs
instead of the ratios and the relight term is deleted. The relight uses the terrain sun direction and a fixed ambient
share (0.55); the lighting lane owns that number.

## 7. Tiers 2-3 (hooks only)

- **Tier 2, localized section swaps.** Hook: `impact::Event` already carries the anchor id, hit point, direction and
  weapon class; a damaged-section consumer keyed on (building part, material class) can subscribe to the same drained
  stream and swap the kit's damaged section, after which decals on that part expire through the anchor test. The kit's
  per-part material class (`materialFromContentClass`: 1 dielectric, 2 painted metal, 3 bare metal, 4 rust) replaces the
  name heuristic when it reaches the runtime. See-through holes come from those sections, not from decals.
- **Tier 3, cosmetic debris (Jolt).** Hook: missile/artillery events with a Static anchor are the debris spawn points;
  debris is presentation-only (never collides with sim objects) and settles into static remnants.
- Bounded battlefield scars (W09): old World decals can be folded into terrain scar tiles by the same pool eviction hook.

## 8. Proof and open items

- Unit tier: `tests/unit/test_impact_decals.cpp` (mapping, budget cap, burst cap, merge, fade/heat, hot-first order,
  mover anchors, box geometry, queue overflow, seed purity, atlas determinism and clean edges).
- Fixture: `MC2_FX_FORCE_SPAWN=impact` fires a ballistic/laser/missile volley at the nearest sheet-metal and concrete
  buildings to the camera focus and at the ground in front, every 1.5 s, through the same queue.
- Open: limb-following mover scars; per-pixel material from the kit (needs a material id in the G-buffer); retiring the
  legacy permanent `CraterManager` craters and the default-OFF `MC2_DYNAMIC_DECALS` ring this supersedes.
