# RUNWAY-LIGHT-MISSING-1 — STATUS

Lane: RUNWAY-LIGHT-MISSING-1. Worktree `A:/Games/mc2-main`, branch `claude/nifty-mendeleev`.
Base `fa8cbf8f`. Deploy lane `mc2-win64-v0.4c` (exe `5dd46496b31c`).

**VERDICT: there is no loss. The runway-light props exist, load, register, classify
as emissive fixtures, and draw. I have photographed them. The lane's founding
hypothesis — "those props are not reaching the static-prop batcher at all" — is
REFUTED, and it was refuted by the very map nobody had looked at.**

**The single most important fact in this report: `mc2_10` places ZERO runway lights.
Every one of the last five lanes verified fixture work on `mc2_10`.** The user's named
subject was never in any of those frames. It is on `mc2_01` (56, day) and `mc2_23`
(112, night) — and on `mc2_23` at night, with the existing default-OFF glow gate turned
up, they light up correctly for the first time in six lanes.

---

## 1. META REVIEW — premises checked against source and data, not status docs

| Premise (from brief) | Verdict |
|---|---|
| "mc2_10 = 2 blue + 2 red + 1 green beacons; NOT ONE runway or taxiway edge light appears anywhere" | **The receipt is right; the inference from it was wrong.** No runway light appears on mc2_10 because **mc2_10 does not place any**. §2. |
| "the runway-light props never reach the renderer at all" | **FALSE.** On mc2_01 and mc2_23 they register and draw. Runtime receipts + photographs, §3–§5. |
| "a 'red beacon' in the receipt could BE a runwaylight misattributed by colour — check identity by asset name" | **Correct method, and it is what cracked this.** `runwaylight.ase` node = `Redbeaconlight03`; `redbeaconlight.ase` node = `Redbeaconlight02`. Distinguishable. §3. |
| `runwaylight.ini` is `0x00ff0000` (red), no white fixture in stock data | **CONFIRMED** by direct read. |
| mc2_03 / mc2_24 register 0 fixtures | **CONFIRMED, and now explained**: they place no beacon or runway fixtures at all. Not a loss — an absence. §2. |
| Overview altitude is where prior attempts died; verify there first | **CONFIRMED as the right discipline** and honoured, §5. |
| mc2_01 is the clean map | **CONFIRMED** — and mc2_23's three runway poses measured a noise floor of **exactly 0**, cleaner still. §5. |
| Contracts 69/69 | **CONFIRMED** at base and after (`69 passed, 0 failed`). |

★ **A premise the brief did not contain, which turned out to decide the whole lane:**
whether each map is *night at all*. The fixture emission is multiplied by `nightFactor`.
`nightFactor` is derived purely from the mission's authored `DayLightPitch`
(`mclib/camera.cpp:2196-2223` — note the `if (day2NightTransitionTime <= 0.0f)` guard is
**commented out**, so this runs unconditionally every update). §2.

★ **A methodological trap I fell into and had to climb out of.** My first pass mapped
placed object type ordinals against `objects/object2.rsp` off by one, and it produced a
beautifully coherent, completely wrong table ("mc2_10 places 32 runway lights"). The
runtime receipt refuted it: mc2_10's receipt names `Redbeaconlight02`, which is
`redbeaconlight.ase`, not `runwaylight.ase`. **The engine's own receipt overruled my
offline parse, which is the correct precedence and is why I ran the receipt before
believing the table.** Corrected mapping: ordinal *N* = line *N+1* of `object2.rsp`
(`<NULL>` is ordinal 0). Every number below uses the corrected mapping and every one of
them is cross-checked against a runtime receipt on three different maps.

## 2. ★ Do the props exist in mission data? YES — and here is every map

Parsed from each mission `.pak`'s terrain-object packet (format per
`code/objmgr.cpp:1276-1323`: `int count`, then 40 bytes/object, `objTypeNum` first),
resolved against `objects/object2.rsp`. `nightFactor`/`isNight` computed by the engine's
own formula from the mission `.fit`'s `DayLightPitch`.

Only missions that place at least one classified fixture are listed (15 of ~30).

| mission | pitch | nightFactor | isNight | **runway** | red | green | blue |
|---|---|---|---|---|---|---|---|
| mc2_05 | 270.0 | 1.00 | **yes** | **22** | 53 | 0 | 0 |
| **mc2_10** | 180.0 | 1.00 | **yes** | **0** | 32 | 12 | 16 |
| mc2_19 | 0.0 | 1.00 | **yes** | **8** | 0 | 0 | 0 |
| mc2_21 | 270.0 | 1.00 | **yes** | 0 | 12 | 0 | 0 |
| **mc2_23** | 9.0 | 1.00 | **yes** | **112** | 69 | 0 | 0 |
| mc2_14 | 20.0 | 0.33 | no | **70** | 8 | 0 | 0 |
| e3demo | 22.5 | 0.17 | no | 4 | 12 | 0 | 0 |
| mc2_11 | 22.5 | 0.17 | no | 0 | 0 | 4 | 0 |
| **mc2_01** | 50.0 | **0.00** | no | **56** | 0 | 0 | 0 |
| mc2_06 | 25.0 | 0.00 | no | 0 | 4 | 0 | 0 |
| mc2_08 | 37.5 | 0.00 | no | 37 | 39 | 38 | 0 |
| mc2_13 | 37.0 | 0.00 | no | 0 | 9 | 9 | 0 |
| mc2_16 | 38.0 | 0.00 | no | 0 | 4 | 2 | 0 |
| **mc2_03** | — | — | — | **0** | **0** | **0** | **0** |
| **mc2_24** | — | — | — | **0** | **0** | **0** | **0** |

**Stock totals: 365 runway lights, 251 red / 74 green / 16 blue beacons.**

Three findings fall straight out of this table:

1. **mc2_10 has no runway lights.** Five lanes' worth of fixture verification was done on
   a map that does not contain the fixture the user named.
2. **mc2_03 and mc2_24 register zero fixtures because they place zero.** The
   predecessor's "0 of 463 / 0 of 489" was never evidence of a bug.
3. **The only stock map that is both night AND densely lined with runway lights is
   `mc2_23`** (112 runway + 69 red beacons, `nightFactor = 1.0`). No prior lane ever
   loaded it. That is where this lane went.

## 3. Where are they lost? NOWHERE — the chain is intact, proved by name at every stage

Traced mission data → object type → appearance → `registerType` → fixture classifier →
draw, and confirmed with runtime receipts on **three** maps. The load-bearing trick is
that `runwaylight.ase` and `redbeaconlight.ase` carry *different* node names, so the
receipt can tell them apart even though both declare identical red `0x00ff0000`:

| asset | ASE node name | receipt shows it as |
|---|---|---|
| `runwaylight.ase` (+ `x`/`dam`/`l1` variants) | `Redbeaconlight03` | `name=Redbeaconlight03 rgb=0xFF0000` |
| `redbeaconlight.ase` | `Redbeaconlight02` | `name=Redbeaconlight02 rgb=0xFF0000` |
| `bluebeaconlight.ase` | `Cylinder01` | `rgb=0x0000FF` |
| `greenbeaconlight.ase` | `Greenbeaconlight01` | `rgb=0x00FF00` |

Receipts captured this lane (`[GPUPROPS] fixture-light type:` lines, gate
`MC2_STATIC_PROP_FIXTURE_LIGHTS` at its default scale 1.00):

| map | placed | receipt | agreement |
|---|---|---|---|
| **mc2_01** | 56 runway, 0 beacons | `Redbeaconlight03` ×2 — 2 of 219 types | ✔ runway only |
| **mc2_10** | 0 runway; 32 red, 12 green, 16 blue | `Cylinder01` ×2, `Redbeaconlight02` ×2, `Greenbeaconlight01` ×1 — 5 of 382 | ✔ beacons only, **no `…03`** |
| **mc2_23** | 112 runway, 69 red | `Redbeaconlight03` ×2, `Redbeaconlight02` ×2 — 4 of 572 | ✔ **both**, distinctly |

Three maps, three different fixture populations, three exact matches between what the
`.pak` places and what the renderer registers. **There is no cull, no skip predicate, no
LOD drop, and no overlay/decal diversion eating these props.** The candidate loss points
named in the brief were all checked and all cleared — in particular
`GHOST-CONE-RECIPE-SKIP-1` keys on the prefix `SpotLight_`
(`gos_static_prop_batcher.cpp:2684`), which none of these node names match.

(The ×2 per fixture is the base mesh plus its damage-state variant — both register, as
expected.)

## 4. ★ So what IS the user seeing? Two different answers, and both are "working as built"

**(a) On day maps the lights are correctly off.** `mc2_01` — campaign **mission 1**, the
one a player meets first, and one of the four tier maps — has a full runway lined with
**56** runway lights and `DayLightPitch = 50` → `nightFactor = 0.0`. Its fixtures are
drawn (photographed, §5d) and their night emission is identically zero *by design*. A
player looking at that runway will never see lit edge lights, and that is correct.

**(b) On night maps they are drawn but never bright.** Photographed on `mc2_23` at
ground level (§5a): the edge lights are there, in two neat rows down the runway — as
small, dull amber cones. Present, but not reading as *lights*. This is exactly the
predecessor's §2 diagnosis, which I confirm rather than re-derive: emission is clamped at
1.0 and then multiplied by a dark lens albedo (`static_prop.frag:436`), so no
emissive-side change can make a fixture bright. At mid and overview altitude they fall
below a pixel and disappear entirely.

**The user's sentence "the actual lights are not on at ALL" is accurate, and it is not a
missing-geometry bug. It is a brightness ceiling on geometry that is present.**

## 5. ★ Measurement — the first time the glow has ever been aimed at a runway light

The existing `MC2_STATIC_PROP_FIXTURE_GLOW` (FIXTURE-GLOW-1, **default-OFF**) was
calibrated entirely on mc2_10's green beacon, because that was the only fixture anyone
could get on screen. I re-ran it on `mc2_23`, where the subject is 112 actual runway
lights. **No engine code was changed for this** — same exe, same gate, different map.

**Noise discipline.** Every number below is against an **OFF-vs-OFF control** from a
separate launch, published alongside the signal. Detector: `ON ≥ OFF on every channel AND
max channel gain ≥ 8/255` — the ≥8 threshold clears the frame-wide ±1 LSB drift the
predecessor documented. `glow3_net` additionally subtracts the control's own gain mask,
so it counts only pixels the glow lit that noise did not.

| pose (mc2_23, night) | altitude | **OFF vs OFF** | glow=1 | glow=3 | **glow=3 net** |
|---|---|---|---|---|---|
| `runway_overview` | **4200** | **0** | **0** | 55 | **55** (0.0007 % of frame) |
| `runway_mid` | 1200 | **0** | 1547 | 1600 | **1600** |
| `runway_ground` | 350 | **0** | 4641 | 4759 | **4759** |
| `beacons_ground` | 350 | 21512 | 11058 | 14753 | 14744 — **unusable, noise ≥ signal** |
| `beacons_mid` | 1200 | 734454 | 745270 | 747784 | 274037 — **unusable** |
| `both_fields_overview` | 6000 | 41647 | 41103 | 40250 | 10736 — **unusable** |

**The three runway poses have a noise floor of exactly zero at all three altitudes** —
the cleanest fixture measurement produced in six lanes, and it is clean precisely because
those poses frame static geometry rather than movers and animated spotlight cones. The
three beacon/wide poses are dominated by mover noise; I am reporting them as unusable
rather than quietly dropping them, because the previous lane's near-miss was exactly this
failure mode.

### (a) OVERVIEW FIRST, as mandated — the bound holds

`runway_overview` at alt 4200 against a **zero** noise floor: **0 px at glow=1, 55 px at
glow=3** on an 8.3 Mpx frame. Eyes-on at alt 6000 (`both_fields_overview`, glow=3): the
airbase reads clean — **no streaks, no over-bright dashes, no repeat of the
"atrociously hideous" failure**. The only bright marks in frame are the pre-existing
spotlight cones, which are unrelated geometry and are present with the gate off too.

### (b) GROUND level — the defect visibly closes

`runway_ground`, noise floor 0, **4759 px** of directional gain at glow=3. Eyes-on: the
edge lights go from dull amber markers to bright white points reading as lit fixtures
along both runway edges. **They desaturate toward white — which the predecessor
correctly predicted is unavoidable in an LDR target, and which for *runway edge lights*
happens to be the physically right answer** rather than a compromise. That is a genuinely
different aesthetic outcome from the pale-green beacon that made the predecessor ship
this OFF, and it is the reason I am surfacing the decision again.

### (c) ★ RED IS NOW OBSERVED GLOWING — the five-lane gap closes

The brief flags that "red and blue have never once been observed glowing across five
lanes". `runwaylight.ini` declares `0x00ff0000` — **these 112 fixtures ARE the
red-declared type**, and §5b is a direct measurement of them responding, against a zero
noise floor, with a photograph. **Red: resolved, works.** Blue remains unobserved
(`bluebeaconlight`, 16 instances, mc2_10 only, and mc2_10's beacon poses are exactly the
noise-dominated ones); I am not claiming it.

### (d) DAY control — correctly zero

`mc2_01` day runway, three altitudes, photographed: fixtures present as geometry, no
emission. The night term is multiplied by `nightFactor = 0.0`, so this is zero by
construction, not by measurement luck.

## 6. What shipped

**No engine change. Nothing in this lane alters a single rendered pixel on a default
launch, and I am not claiming a fix.** What shipped is the thing that was actually
missing — the ability to see the subject at all:

- `tests/visual/bookmarks/mc2_23_runway_lights.json` — 6 poses (ground/mid/overview over
  both the runway-light and beacon fields) on **the only stock map that is night AND
  runway-lit**. This is the fixture whose absence let five lanes verify on mc2_10.
- `tests/visual/bookmarks/mc2_01_runway_lights.json` — 3 day-side control poses.
- This document, including the per-mission placement × night-state table (§2), which is
  the reusable artefact: it says which map to open for any future fixture work.

Both bookmark files carry the derivation in their `_note` so the next reader does not
have to re-parse a `.pak` to trust them.

## 7. ★ The decision this hands back to the user

The mechanism exists, is bounded, is verified at overview, and is **default-OFF**. The
predecessor shipped it OFF because on a green beacon it looked pale rather than lit, and
said the one thing it needed was the user's eye on a strength. That judgement was made on
the wrong fixture. On the right fixture — 112 runway edge lights on mc2_23 at night — it
does what the user asked for.

The concrete question, worth five minutes and no further lane:

> `MC2_STATIC_PROP_FIXTURE_GLOW=3`, mission **mc2_23**, ground level over the runway.
> Do those read as runway lights? If yes, this gate should flip default-ON, and the
> five-lane defect is closed.

I did not flip it myself. It is an aesthetic call on a user who has already rejected one
attempt in this area, the evidence I have is from a scripted camera rather than play, and
a default flip is exactly the move that has failed here before.

## 8. Verification performed

- Runtime receipts on **mc2_01, mc2_10, mc2_23** (§3) — placement vs registration agrees
  exactly on all three.
- `scripts/check-contracts.sh` → **69 passed, 0 failed**.
- Captures ran on deployed exe `5dd46496b31c`; worktree HEAD is `fa8cbf8f`. **The single
  commit in `5dd46496..fa8cbf8f` is `fa8cbf8f`, which is docs-only
  (`.claude/FIXTURE-GLOW-1-STATUS.md`, 1 file changed).** Verified by `git log` over
  `gos_static_prop_batcher.cpp`, `static_prop.vert/.frag` and `camera.cpp`: **zero
  commits touch the measured path**, so the exe is functionally identical to HEAD and no
  rebuild could change a number in this report. The fingerprint mismatch is real and
  disclosed; it is also provably immaterial.
- `MC2_STATIC_PROP_FIXTURE_GLOW`: `gate_status` → **verdict OK, flags []**, registered ✓,
  tier1-documented ✓, smoke-allowlisted ✓, default OFF. Unchanged by this lane.
- All captures `--no-kill`; no foreign PIDs touched. No push, no `--amend`.
- Disk watched throughout (A: held at 5.7 GB free); every PNG verified non-zero (all
  24,887,323 B 4K frames) before analysis.

## 9. Adversarial self-review

- *"Would the user still see the defect on a default launch?"* — **Yes.** Default launch
  is byte-identical; the glow gate is still OFF. I fixed no pixels. What I closed is the
  *diagnosis*: the props are not missing, and the map to look at is mc2_23.
- *"Did you verify at overview first?"* — Yes, and it is the strongest row in the table
  (0 px at glow=1 against a 0 px floor at alt 4200, plus a clean eyes-on at 6000).
- *"Is 'no loss' proved or asserted?"* — Proved three times over, by name, on three maps
  with three different fixture populations, using a discriminator (`…03` vs `…02`) that
  can distinguish two assets sharing an identical declared colour. A single map would
  have been an assertion.
- *"You claim mc2_10 has no runway lights — but your first table said 32."* — That first
  table was wrong by a one-line ordinal shift and I have said so in §1 rather than
  quietly correcting it. The runtime receipt is what caught it, and the corrected mapping
  now reproduces all three receipts exactly, which the wrong one could not.
- *"Where are you weakest?"* — Three places. (i) **Blue is still unobserved** and I
  cannot fix that from mc2_23, which has no blue; it lives on mc2_10 whose beacon poses
  are noise-dominated. (ii) Three of my six mc2_23 poses are **unusable** — I framed them
  around movers and animated cones, and I report them as failures rather than excluding
  them silently. (iii) The §7 aesthetic judgement is mine from a static frame; the user
  should overrule me freely, in either direction.
- *"Anything self-serving in the framing?"* — The honest summary is that this lane
  **found no bug**, and a lane commissioned to find a bug reporting "there isn't one"
  should be read with suspicion. The counterweight is §2: the placement table is
  falsifiable in one command, and it independently explains every fixture receipt any
  lane has ever produced, including the two the predecessor could not explain
  (mc2_03 = 0 and mc2_24 = 0).
