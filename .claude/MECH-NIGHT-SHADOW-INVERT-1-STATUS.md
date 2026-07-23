# MECH-NIGHT-SHADOW-INVERT-1 — status (2026-07-23)

Master-FAIL #7 (MED): "Inverted mech night shadows (shadow BRIGHTER than
ground)". Branch `claude/nifty-mendeleev`, worktree A:/Games/mc2-main.
Commit `5b12ddd7`. Gate `MC2_MOVER_CONTACT_SHADOW` (default 0.0 = OFF).

## Root cause (evidence-backed, NOT the legacy blob path)

The legacy CPU blob shadow (`mech3d.cpp` `renderShadows` / `mechShadowShape`)
is SKIPPED whenever `gos_IsModernShadowPathActive()` (shadow_terrain material
loaded — always true in v0.5.1), so it is NOT the culprit. `MC2_GROUND_CONTACT_BLOB`
is default-OFF and packs pure black (can't brighten) — also not it.

The real mechanism is the **mover-attached light pool** in the CHUNK terrain
path (`terrain_lod_chunk.frag`, light-pool SSBO binding 28; the legacy/tess
`gos_terrain.frag` has no pools). Each mover registers a light pool at the
mech's position. The falloff is `1.0` for every fragment within the pool's
close radius (`len <= nearD`), so a mech lights the ground **brightest directly
beneath its own body** — a bright warm disc exactly where a contact shadow
belongs. The CSM sun-cast shadow at the clamped low night sun is a thin side
sliver, invisible on near-black night terrain, so nothing counters the disc.
Net read = a light-grey/warm fan **brighter** than surrounding night terrain.

Region ground-truth (`tools/visual_verify/regions/mc2_05_saved_205843.json`,
`mech_night_shadow_fan`): "mech ground shadow renders as a light-grey triangular
FAN ~2x BRIGHTER than the night terrain it sits on (fan ~[22,24,18] vs terrain
~[9,11,11]). Should darken, not lighten." Independent pixel sample (nightpools
night_on capture, left mech feet): ground `[43,43,35]` vs surround `[11,13,13]`;
identical with light-pools gate off/on for that mover-glow-independent measure —
confirming the bright patch is the mover pool falloff, not the NIGHT-LIGHT-POOLS
building term.

## Fix (MOVER-CONTACT-SHADOW-1, gated, chunk-terrain only)

Files: `shaders/terrain_lod_chunk.frag`, `GameOS/gameos/gos_terrain_lod_chunk.cpp`
(chunk path owns its own pool-uniform upload — NO `gameos_graphics.cpp` edit),
`scripts/run_smoke.py` + `scripts/check-env-registry.sh` (gate registration).

For **mover** pool lights only, when `u_moverContactShadow > 0`:
1. carve the bright glow center into a ring — `falloff *= mix(1.0, ring, k)` with
   `ring = smoothstep(0, nearD, len)` (0 under the body, 1 at the footprint edge):
   the mech no longer floodlights the ground it stands on;
2. accumulate the strongest under-body occlusion `moverContact` and, after the
   full lighting composite (base + sun + pools + padGlow + hemi), apply
   `lit *= (1.0 - 0.55*moverContact)` so the contact patch reads DARKER than the
   surrounding ground — a real night contact shadow, not merely a neutralized disc.

Gate `MC2_MOVER_CONTACT_SHADOW` in [0,1]; recommended ~0.7. Default 0.0 →
`mix()` collapses to 1.0 and `moverContact` stays 0 → the `if (moverContact>0.0)`
guard is false → **provably byte-identical** to stock (no macro, plain uniform
uploaded as 0). Buildings / terrain / spot pools untouched.

## Merge safety
Narrow: one shader + its CPU uploader + two registry lines. No overlap with the
runway-edge-blend lane (cement branch) or batch-8C (gameos_graphics.cpp).
Default-OFF byte-identical → survives wave-3 merge inert; flip the gate on in the
launch config when the look is user-approved.

## Verification
Build `98ed1d32` (RelWithDebInfo, exit 0), deployed via `deploy_payload.py` to
canonical lane `mc2-win64-abl-validate`. Headless capture at
`mc2_05_saved_205843` (true night) via `tools/visual_verify/capture.py`,
gate OFF vs `MC2_MOVER_CONTACT_SHADOW=0.7`. Evidence:
`A:/Games/mc2-main/scratchpad/mns_off|mns_on|mns_off2` (+ brightened crops
`scratchpad/mns_cmp_off.png` / `mns_cmp_on.png`).

- **Default-OFF byte-identical: PROVEN by construction.** uniform uploads 0.0 →
  `mix(1.0,ring,0.0)=1.0` (falloff untouched) and `moverContact=(1-ring)*0.0=0`
  → `if (moverContact>0.0)` false → exact stock output. Plain uniform, no macro.
- **Gate-ON darkens the mover ground glow (directionally correct).** The change
  is spatially TIGHT and centered on the mover (robust-signal bbox x302-417,
  y165-301), reducing the affected pixels ~20% (`[107,112,103]→[84,90,83]`); a
  denser sub-cluster drops ~36%. No global shift — buildings/terrain away from
  movers unchanged.
- **CAVEAT (honest):** this bookmark frames a single DISTANT mech, and the pose
  is NOT byte-deterministic across separate launches (moving cloud shadows: a
  ~2071px whole-frame noise floor between two OFF runs). At this weak framing the
  fix's few-hundred-pixel signal overlaps that animation noise, so the pixel A/B
  is directional, not a clean isolated diff. A dramatic A/B needs the close
  3-mech night group (the nightpools saved-game framing where the fan measured
  `[43,43,35]` warm vs `[11,13,13]` terrain) — not reproducible from this camera
  bookmark. Mechanism + region ground-truth + code-correctness carry the verdict;
  final look-tuning of the 0.7 strength is a user eyeball, exactly as the sibling
  NIGHT-LIGHT-POOLS gates were shipped default-tuned then user-graded.
- Day regression: gate default-OFF ⇒ day paths byte-identical (same proof);
  the term is night-only in effect anyway (mover pools are a night feature).

## Scope-add verdict — palm vs pine day/night shadow (fleet LOW #10)
NOT a shader bug and NOT in this fix. The shadow depth passes
(`shadow_object.frag`, `shadow_instanced.frag`, `shadow_depth.frag`) write depth
only with **no alpha discard**. Because alpha-card foliage would therefore cast
solid rectangular blob shadows, far/impostor LODs are deliberately excluded from
shadow casting (`bdactor.cpp:7169-7172`, `GpuStaticPropRegistry::setRecipeNoShadow`).
Palms whose canopy is thin alpha cards (or whose LOD0 presents edge-on to the
high day sun) thus cast negligible shadow; pines are solid cone geometry and cast
solidly. The engine treats both uniformly — the difference is purely source-mesh
geometry meeting the no-alpha-discard shadow pass. A real fix = add alpha-test to
the shadow depth FS + re-enable casting for those recipes (a separate slice, not
narrow). Recommend a dedicated SHADOW-FOLIAGE-ALPHATEST slice; out of scope here.
