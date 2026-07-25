# CONTACT-SHADOW-ALLTYPES-1 — status (2026-07-23)

Extend the Master-FAIL #7 fix `MC2_MOVER_CONTACT_SHADOW` (commit `5b12ddd7`,
ring-carve of a mover's pool-light falloff + under-body contact-AO in
`shaders/terrain_lod_chunk.frag`) to ALL relevant object types. Branch
`claude/nifty-mendeleev`, worktree A:/Games/mc2-main. Commit `9590805f`. Gate
`MC2_MOVER_CONTACT_SHADOW` (default 0.0 = OFF, byte-identical) — unchanged; no
new env var.

## How pool lights are populated per object class (investigation)

Pool lights are packed into the terrain light-pool SSBO (binding 28) by
`gos_terrain_lighting.cpp` (`PackAndDispatch`, ~L876-984). A light is flagged as
a **mover** (`GpuTerrainLight._pad1 = 1`, read in the frag as `_plPad1`) iff its
world-light slot was tagged with `mc2_spotlight_diag::Mech` **or** `Gv`
(L904-906). There are exactly THREE registration sites / source classes:

| Class | Registration site | mover flag | Contact-shadow term |
|---|---|---|---|
| **Mech** (incl. helicopters) | `mech3d.cpp:4077/4176` → `Mech` | YES | applies |
| **Ground vehicle** | `gvactor.cpp:2653` → `Gv` | YES | applies |
| **Building** | `bdactor.cpp:3247` → `Bldg` | NO | skipped |

Helicopters are NOT a separate actor class — `mech3d.h:315 isHelicopter`
(`= isAirborne`, `mech3d.cpp:2475`); they flow through the Mech appearance and
register the same SLCircle/SpotLight pool light, tagged `Mech`. Elementals have
no light-registration actor of their own (only `sounds.h`/`dobjclass.h` refs) —
they ride the ground-mover path. So the ONLY per-class variable is the tag, and
the original #7 shader term already reaches every light whose tag is Mech or Gv.

## Per-class verdicts

- **Ground VEHICLES (Gv): COVERED by the original #7 commit — no code change.**
  `gvactor.cpp` already tags its pool light `Gv`, which the pack loop already
  puts in the mover set, so vehicles already received the ring-carve + contact-AO
  from `5b12ddd7`. Their SLCircle light sits ~ground (|vtl.z|≈0) so the new
  altitude fade leaves them at full strength (altFade=1.000) — identical behavior.

- **HELICOPTERS / airborne movers: FIXED (the one real gap).** Helos carry the
  Mech mover flag, but their SLCircle pool light rides at flight altitude. The
  original term applied the FULL contact-AO regardless, gluing a full-strength
  dark disc to the ground beneath a hovering helo. Added an altitude fade keyed
  to the light's height above the ground fragment (`abs(vtl.z)`, data already in
  the pool SSBO): `altFade = 1 - 0.8*smoothstep(20,70,|vtl.z|)`. Ground movers
  (|vtl.z|≤20) → altFade=1.000 (unchanged); airborne helo (|vtl.z| 45→70+) →
  0.60→0.20 = a soft faded blob at the ground point, not a glued dark disc. The
  ring-carve of the bright glow center is deliberately left un-faded (a helo
  shouldn't floodlight bright beneath itself either); only the DARK contact-AO is
  softened. Because it only reduces a darkening term it cannot introduce a bright
  artifact. Purely inside the existing `u_moverContactShadow>0` guard → default
  OFF still byte-identical.

- **BUILDINGS (Bldg): NOT AFFECTED — deliberately skipped (documented).**
  Building lights are excluded from the mover set (`_pad1=0`), so neither the old
  nor new term touches them. Extending the ring-carve to buildings would carve
  their footprints DARK, directly regressing the shipped, user-approved
  `PAD-AREA-GLOW-1` feature (which was ADDED specifically to LIFT the pad around
  buildings because pitch-dark building pads read as pasted-bright cutouts).
  Buildings also don't exhibit the reported defect (#7 is specifically inverted
  MOVER shadows) and need no mover-follow. Verdict: skip. Visually confirmed:
  the mc2_10 guard-tower footprints keep identical lit pools OFF vs ON
  (`scratchpad/cs_alltypes/AB_buildings_212019_OFFvsON.png`).

## Fix (one file)

`shaders/terrain_lod_chunk.frag` (~L1729), inside the existing mover branch:
```glsl
float altFade = 1.0 - 0.8 * smoothstep(20.0, 70.0, abs(vtl.z));
moverContact = max(moverContact, (1.0 - ring) * u_moverContactShadow * altFade);
```
No C++ change (the mover classification in `gos_terrain_lighting.cpp` already
covers mechs+vehicles+helos correctly; buildings are correctly excluded).

## Verification (build `9590805f`, RelWithDebInfo exit 0, deployed
`deploy_payload.py` → `mc2-win64-abl-validate`, manifest src_commit `9590805f2bc8`)

- **Shader compiles + all night scenes render intact.** Headless sweep of 5
  night bookmarks (mc2_05 + 4× mc2_10 airbase/compound/valley) at gate OFF and
  gate ON=1.0 (exaggerated): 5/5 fired, 5/5 deterministic, terrain + building
  pools render with no artifacts. Evidence: `scratchpad/cs_alltypes/off|on/`.
- **Default-OFF byte-identical:** proven by construction (uniform uploads 0 →
  `moverContact` stays 0 → `if(moverContact>0)` false). Confirmed empirically on
  mc2_10_saved_212140 (no close mover in frame): OFF vs ON = 0 changed pixels.
- **Ground movers unchanged:** altFade math table shows altFade=1.000 for
  |vtl.z|≤20 (mech/vehicle SLCircle height) — the verified #7 behavior is
  preserved bit-for-bit for ground movers.
- **Buildings unaffected:** visual A/B crop of the mc2_10 guard-tower footprints
  is identical OFF vs ON (only a moving unit's selection marker differs).
- **CAVEAT (honest, same limitation as #7):** the available night bookmarks are
  all distant OVERVIEW poses — none frame a close hovering helicopter or a close
  ground vehicle, and this build has no `MC2_TEST_SPAWN` verb to stage one
  headless. So the dramatic per-class close-up contact A/B (helo soft-blob vs
  ground-mover full-disc) is carried by the shader-math proof + code-correctness,
  not a pixel crop — exactly as the sibling #7 mech A/B was shipped directional
  then user-graded. The full-frame OFF/ON diffs on mc2_10_204753/212019 are
  cross-launch scene animation (sweeping searchlights, water, moving units), not
  the contact term (bboxes span the whole frame with mixed darker/brighter; a
  localized contact-AO would be darker-only and mover-tight). Final look-tuning of
  the fade thresholds (20/70 wu) and the 0.7 strength is a user eyeball.

## Merge safety
Narrow: one shader hunk inside the existing #7 gate. No C++ change, no new env
var, no new uniform. Default-OFF byte-identical → survives wave-3 merge inert.

---

## RECAP on mc2_10 (2026-07-24, lane RECAP-MECHSHADOW-RUNWAY-1)

User correction: the v0.5.2 review pack shipped the #7 A/B captured at
**mc2_05**, not mc2_10, and asked why. Answer + fix below. No code change —
this is a pure evidence recapture against exe `ea999e8b` (contact-shadow
source unchanged since `9590805f`).

**Why mc2_05 was used.** Every saved mc2_10 night bookmark (212019, 205952,
204907, 204611, 212140, 204753) is a distant OVERVIEW. 212019 — the lowest of
them at alt 2113 — frames guard towers and tree line only; no mech appears at
readable size in any of them. The original capture fell back to the mc2_05
bookmark because that one happened to contain a (distant) mech.

**How the pose was composed instead of settling.** The CAVEAT in the section
above ("no verb to stage a mover headless") still holds — the
`MC2_UI_COMMAND_FILE` channel has only `toggle_pause` / `press pause.*` /
`press dialog.*` / `debugview` / `screenshot` verbs, so no select/order/camera
driving is possible. But the movers are already there at mission start: reading
`data/missions/mc2_10.fit` gives the player lance at `CommanderID = 0`,
`PositionX = -4842.666504`, `PositionY = -5525.333984`. Composing a bookmark at
that world position (rot 150, proj 20, alt 900, z 200) frames a player mech
large, at night, standing in its own light pool. Pose archived at
`release_assets/0.5.2/REVIEW/03_mech_night_shadow/mc2_10_lance_close.bookmark.json`.

**Result (3840x2160, gate OFF vs `MC2_MOVER_CONTACT_SHADOW=0.7`).**

| Check | Verdict | Evidence |
|---|---|---|
| mc2_10 night pose framing a mech at readable size | PASS | composed `mc2_10_lance_close`; mech ~700 px tall |
| Cross-launch noise floor (OFF vs OFF) | PASS | 16 px changed, max delta 3 |
| Contact term fires, and DARKENS | PASS | crop 1700x900 at the mover: 44.6% px changed, **71% darker**, mean dLum -3.10, max delta 223 |
| Full-frame OFF vs ON | PASS | 1.275M px, bbox spans the whole light pool (ring-carve) |
| Default-OFF byte-identical | PASS | unchanged by construction; OFF/OFF pair above |

**Honest residuals.**
- The mech **idly rotates its torso** and that animation is not launch-stable
  (two ON captures differ by 819k px from the torso alone, while two OFF
  captures differ by 16 px — the ON arms happened to land on different
  animation phases). The shipped crop is therefore framed on the **legs and
  ground**, which are pixel-stable. This is correct framing on the merits: the
  fix is a terrain-shader effect and the mech body carries none of it. A
  torso-matched full-body pair would need a frame-exact anim clamp that this
  build does not expose.
- **No VEHICLE could be framed**, same reason as before: no order/select verb,
  and mc2_10's vehicles are off-map staged until objective-gated
  (INVIS-VEHICLES-FIX-1 census). The `Gv` class stays carried by the
  code-path argument, not a pixel crop. Unchanged from the original caveat.
- Review pack section 3 images + text replaced; the WEAK-CAPTURE flag is
  removed (the capture is now clean and on the right map), and replaced with
  the two narrower flags above.
