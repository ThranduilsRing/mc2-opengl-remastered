# LIGHT-REALISM-FLIP-1 — STATUS

**Verdict: DID NOT FLIP.** `MC2_LIGHT_REALISM` stays default-OFF.
Three of the user's four night-lighting complaints are convincingly solved by
variant B. The fourth ("huge blown-out white pool") is **made measurably worse**,
and that item is the one the user raised most recently and most bluntly.

Evidence exe: `48e95f43ecc6`, `dirty=0`, branch `claude/nifty-mendeleev`
(`[BUILD_FINGERPRINT v1]` confirmed in every capture log). Deploy lane `0.4c`.
No source changes made; tree left clean.

## Grade against the user's own words

| User quote | Verdict | Evidence |
|---|---|---|
| "the cones are ultra visible, they need to be feathered / more transparent" | **SOLVED** | `mc2_10 saved_212019`. OFF = two hard-edged pale slabs with crisp straight silhouettes. ON = soft-edged shafts with a bright core and no hard rim. Decisive. |
| "the light needs to spill from there, not be just in random spots" | **SOLVED** | `mc2_10 saved_204753` / `204958`. OFF = blotchy pools with dark ground between fixture and its lit patch. ON = the base reads as one continuous illuminated compound with a soft outer falloff. |
| "the light sources need to mimic LIGHT SOURCES (real ones)" | **SOLVED** | `mc2_10 saved_204958`. `QueueBeaconGlow` is gated on `realism_enabled()`, so ON is what turns the runway edge markers from 1-2px hard dots into red/green beacons with halos. |
| "terrible lighting here" (huge blown-out white pool, hard white cone slabs over five small units) | **REGRESSED** | Cone half fixed; **pool half worsened.** See below. |

## The blocker, quantified

`MC2_LIGHT_REALISM_POOL_SPILL=2.2` scales `farDistance` for **every** pool light
uniformly (`gos_terrain_lighting.cpp:1338-1348`), including mover-attached
(mech / elemental) lights, which sit closest to the camera and are already
near-saturating. The pool gets wider and softer at the *rim*, but the
near-saturated *area* grows.

"hot" = pixels with mean RGB >= 220. Full-white clipping (all channels >= 250)
stays small in absolute terms; the readable failure is the hot area.

| mc2_10 pose | hot% OFF | hot% ON | factor |
|---|---|---|---|
| `lance_close` | 0.360 | **8.250** | 23x |
| `saved_204937` | 0.449 | 2.187 | 4.9x |
| `saved_125552` (user grading pose) | 0.067 | 0.764 | 11x |
| `saved_204753` (user grading pose) | 0.148 | 0.925 | 6.3x |

At `lance_close` the pool swallows the whole foreground: terrain texture and
night contrast are washed out edge to edge. That is the same failure mode the
user named, arriving from the opposite direction.

## Noise floor (measured this lane, A vs A2, byte-identical env)

| map | floor (changed px %) |
|---|---|
| mc2_01 | 0.035 |
| mc2_03 | 0.049 |
| mc2_05 | 1.54 |
| mc2_10 `lance_close` | 3.00 |
| mc2_10 `saved_125552` | 0.87 |

Published floors for the remaining mc2_10 poses: `docs/visual-verify.md`
(`saved_204753` = 9.1 %). All effects claimed below are above their floor.

## ON vs OFF, all maps (changed px %, mean luminance delta)

- **Day is untouched.** mc2_01 `0.003 %`, mc2_03 `0.043 %`, mc2_24 `0.017 %` —
  at or below each map's own noise floor. The gate is night-only; no day risk.
- **mc2_10 night**: 2.7 % - 59.5 % changed, mean dL `+14` to `+61`, 98-100 % of
  changed pixels brighter.
- **mc2_05 (USER-APPROVED night)**: `10.57 %` changed, mean dL `+31.3`,
  98.4 % brighter, against a `1.54 %` floor. **This is a real change to content
  the user already signed off on** — I judge it an improvement (the distant town
  reads as a lit settlement rather than isolated dots) but I cannot claim it is
  undegraded on my own authority. It needs the user's eye.

## Fix direction (identified, NOT implemented, NOT verified)

The pool shader already distinguishes movers — `gl._pad1 = lightIsMover(tl)`
(`gos_terrain_lighting.cpp:1351`) is fed specifically so the frag pass can dim
mover-attached lights. Applying the spill scale to **static (building/tower)
lights only**, leaving mover lights at stock radius, would keep all three wins
and remove the regression. That is a code change rather than a constant, so it
is out of scope for a pure default-flip lane and is unverified.

`MC2_LIGHT_REALISM_CONE_OPACITY=0.15` (the later lane's candidate) was **not
graded** — see blockers. It targets cone harshness, which the measurements say
is already solved at variant B's `0.40`; it does not address the pool.

## On the additive-saturation theory (asked explicitly)

**A per-fragment accumulation clamp is not the lever, and is not cheap.** Cone
tris are queued into the shared `MC2_ISSPOTLGT` batch and drawn by a plain
`gos_RenderIndexedArray` loop with ZWrite off (`mclib/txmmgr.cpp:4155-4200`) —
there is no cone-specific fragment stage to clamp in. Clamping accumulated
contribution would require a separate render target plus a tone-map resolve.
It is also unnecessary: the screen-space silhouette feather already fixed the
cone appearance at variant B's constants, as `saved_212019` shows directly.

## Blockers hit (both real, both need attention)

1. **The A: volume is 100 % full — 0 bytes free.** This is what actually killed
   the second half of the matrix, not lane contention: 4 capture runs failed at
   76 s and 12 of 14 `A2_OFF_REPEAT/mc2_10` PNGs were written zero-byte. It
   blocks every lane on this machine, not just this one. Freeing my own lane's
   junk recovered only 53 MB. Costs: the mc2_10/mc2_24 noise-floor repeat and
   all three tunable arms (`X_EXAG` verify-by-exaggeration, `C_CONE015`,
   `D_NOFEATHER`) are **unrun**. Verify-by-exaggeration was required and was
   not performed.
2. **`mc2-main` HEAD moved under this lane** — `48e95f43` -> `6c5c60c2`
   ("fixture-identity-scoped night emission for runway/beacon lights",
   sibling lane BEACON-FIXTURE-ID-1), and the 0.4c exe was replaced at
   06:17:35Z mid-run. That commit touches the beacon/runway path — i.e. exactly
   the third win graded above — so the beacon behaviour may already differ from
   what was measured here.

## What a follow-up lane must do

1. Free disk on A:. Nothing else is runnable until then.
2. Re-grade on top of `6c5c60c2`, since the beacon path moved.
3. Run the unrun arms: exaggeration, `CONE_OPACITY=0.15`, feather-off isolation,
   plus the mc2_10/mc2_24 noise-floor repeat.
4. Prototype the static-only spill carve-out and re-measure `lance_close` hot%.
5. Show the user `mc2_05` ON vs OFF explicitly — it is above noise and they
   already approved the OFF look.

## Artifacts

`scratch/lrflip1/` — `drive.py` (capture matrix), `compare.py`, `compare.json`,
`drive.log`, `out/` (A_OFF, B_ON, partial A2 across mc2_01/03/05/10/24),
plus the 2-up graders `zoom_saved_212019.png` (cones — the decisive one),
`zoom_saved_204958.png` (beacons), `crop_saved_204753.png` (base spill),
`zoom_lance_close.jpg` (the regression).
