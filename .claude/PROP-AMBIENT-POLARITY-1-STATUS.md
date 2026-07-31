# PROP-AMBIENT-POLARITY-1 — STATUS

Lane: PROP-AMBIENT-POLARITY-1 (eighth lane on the dark-prop defect)
Base: claude/nifty-mendeleev @ 6440c84c
Outcome: **RECON ONLY — no code shipped.** The polarity question is settled
with a hard receipt, and it INVERTS the defect premise. No bake bug exists to
fix; the real gap is enumerated below and needs its own lane.

---

## 1. POLARITY — SETTLED WITH AN EXACT RECEIPT

**`0.7529` is the STALE BOOT value. `0.2471` is the SETTLED mission value.**
The refuting lane's suspicion was correct. The 78 still-bright props are the
anomaly; the dim props are lit as authored.

### Receipt (mission data, not inference)

`mc2_10.fit` (verified identical in `mc2-opengl/mc2srcdata-fresh/missions/`
and `releases/mc2-win64-v0.5.0/mods/MCO-Carver5/data/missions/`):

```
uc AmbientRed   = 192      uc DayAmbientRed   = 192      uc NightAmbientRed   = 32
uc AmbientGreen = 192      uc DayAmbientGreen = 192      uc NightAmbientGreen = 32
uc AmbientBlue  = 192      uc DayAmbientBlue  = 192      uc NightAmbientBlue  = 32
uc DayLightRed  = 255                                    uc NightLightRed     = 64
```

**192 / 255 = 0.752941 = the measured `0.7529`, to every digit captured.**

Mechanism, end to end:

1. `mclib/camera.cpp:423` — at mission LOAD: `ambientRed = dayAmbientRed;`
   → `ambientRed = 192`.
2. `mclib/camera.cpp:2539` — `DWORD lightRGB = (ambientRed<<16)+...;
   eye->setLightColor(1, lightRGB);` — `worldLights[1]` is the AMBIENT light
   (`camera.cpp:516`, `worldLights[1]->init(TG_LIGHT_AMBIENT)`).
3. `mclib/txmmgr.cpp:2385-2390` (`GatherLightsParameters`) — packs each ACTIVE
   light into a row, `lightDir[k][3] = (float)type`, `lightColor[k][*] =
   byte/255.0f`. `worldLights[0]` = sun (INFINITE) → row 0;
   `worldLights[1]` = ambient → row 1.
4. `shaders/static_prop.vert:~712-720` and the CPU mirrors
   (`txmmgr.cpp:1827-1832`, `bdactor.cpp:3224-3238`) sum every AMBIENT-typed
   row into `ambientTerm`. With `nl=2` that is exactly row 1 = `ambientRed/255`.

So `amb` **is** `ambientRed/255` and nothing else.

- **Frame 0** — `Camera::updateDaylight` has not run yet, so `ambientRed` is
  still the load-time `dayAmbientRed = 192` → `0.7529`. **STALE BOOT.**
- **Frame 1+** — day/night calc has run and `ambientRed` is the mission's
  settled value → `0.2471` (= 63/255). **SETTLED.**

This also explains the previously-unexplained frame 0→1 split with sun state
and light count held fixed: nothing about the sun or the tree window changes
at that boundary — `ambientRed` does.

### Consequence: LIGHTBAKE-REBAKE-ON-READY-1 is CORRECT

The soft invalidation converting 172 of 250 bright rows to dim is the
rebake-on-ready fix doing precisely its job: evicting boot-baked rows that
froze the 192 day ambient and re-gathering the settled value. **It is not the
bug. It is the repair.** Any lane that "fixes" props by restoring 0.7529 would
be re-freezing the stale boot ambient across the whole scene.

### One soft spot, stated honestly

`0.2471` = 63/255, which is NOT the raw `NightAmbientRed = 32` (= 0.1255). The
natural fit is the dusk interpolation at `camera.cpp:1873`:
`fAmbientRed = (dayAmbientRed - nightAmbientRed) * fSineOfPitch + nightAmbientRed`
= `(192-32)*s + 32`, which equals 63 at `s = 0.194` — a low but not-yet-set
sun. **I did not independently confirm mc2_10's light pitch yields s≈0.194.**
That does not weaken the polarity ruling (which rests on the exact 192 match
and the load-order mechanism), but the precise provenance of 63 is one step
short of a receipt. Confirming it is cheap: log `ambientRed` per frame.

---

## 2. THE REAL QUESTION — WHAT A BUILDING GETS THAT A TRUCK CANNOT

Since the dim ambient is correct, "lit building, black truck at identical
ambient" must come from a term that is not ambient. Enumerated from
`shaders/static_prop.vert`:

Final prop colour = `seed(perVertexARGB) + ambientTerm + sunColor*NdotL`
(+ optional emissive/glow/IBL).

| Additive term | Source | Can a parked truck receive it? |
|---|---|---|
| `ambientTerm` | AMBIENT rows | **Yes** — identical, `0.2471`. Not the differentiator. |
| `sunColor * NdotL` | first INFINITE row × `u_terrainSunMC2` | Yes, but at night `NightLight=64` and NdotL≈0 on most parked-vehicle facets. Near-zero for both. |
| **night emission** (`get_base_light` hot-pink/yellow/green tags) | per-vertex ARGB tags, `u_nightEmissiveScale` / `u_fixtureEmissiveScale` | **NO.** These are stock *window//lit-panel* vertex tags. Buildings carry them; `crateswithtarps01`, `JeepProp`, `Barrels` do not. Additive and computed BEFORE ambient, so night ambient never darkens it. |
| **fixture glow** (`v_fixtureGlow`) | `isFixture = ptd.hotPinkRGB.w > 0.5` | **NO** — gated on fixture identity. |
| **IBL SH probe** | `u_iblSh[9]`, `u_iblShStrength` (`static_prop.vert:243-244, 845-847`) | Yes — applies to all static props by normal. Not a building/truck discriminator. |
| **point / spot light pools** | — | **NO — AND NEITHER DOES A BUILDING.** |

### The structural finding

`shaders/static_prop.vert` has **no point or spot light handling whatsoever**.
`grep -n 'TG_LIGHT_POINT\|TG_LIGHT_SPOT' shaders/static_prop.vert` returns
**zero matches**. The light-scan loop accumulates only AMBIENT and the first
INFINITE; the source comment is explicit:

> "Point/spot lights are omitted (negligible for buildings); noted."

That assumption was written for *buildings*, and for buildings it holds —
buildings self-illuminate via their window/emissive vertex tags, so they never
needed the pools. **It does not hold for props with no emissive tags.** A truck
parked inside a lit airbase compound receives nothing from any fixture light:
the terrain beneath it is lit by `terrainLights`, the building beside it is lit
by its own window emission, and the mech beside it goes through an entirely
different path (`gos_mech_batcher`) that does consume the pools.

That is a coherent, mechanism-level explanation of the exact frame the user
reported, and it is consistent with the user's ruling ("it is the lighting
path") — it is a *missing* lighting path, not a corrupted one.

---

## 3. WHAT I DID NOT DO

- **No code shipped, no gate added, no build, no deploy, no smoke.**
  There is no `DEPLOY_FINGERPRINT OK` and **no tier1 claim** in this report.
- **On a DEFAULT launch the user will still see the defect**, unchanged.

I stopped deliberately. Seven lanes died chasing the bake; the eighth shows
there was never a bake bug — the bake is correct and the invalidation is the
repair. Shipping a prop-lighting change on top of that finding, in the same
lane that produced it, is how lane nine gets written.

---

## 4. RECOMMENDED NEXT SLICE — `PROP-POINT-POOL-1`

Give static props the point/spot pools that `static_prop.vert` structurally
omits, so unemissive props inside lit compounds receive fixture light.

- Extend the `static_prop.vert` light scan to handle `TG_LIGHT_POINT` /
  `TG_LIGHT_SPOT` with the existing `GetFalloff` fields (already populated for
  cache uniformity — `txmmgr.cpp:2399-2402` — so the data is present today and
  simply unread).
- Gate `MC2_STATIC_PROP_LIGHT_POOLS`, **default-OFF**; register in
  `RendererFeatureRegistry.h`, document in `docs/tier1_env_vars.md`, add to the
  `run_smoke.py` passthrough allowlist and `check-env-registry.sh` ALLOWLIST
  (all four, or the gate-ON smoke silently runs gate-OFF).
- Grade at the user's mc2_10 night compound + intro poses, two-run floor.
- Watch cost: this reintroduces per-prop light-pool work on a path deliberately
  reduced to a constant by LIGHTBAKE v2. Baked rows are frozen per recipe, so a
  moving/flickering fixture will NOT update a prop's row without invalidation.
  Scope to static fixture lights first.

### Cheap confirmations worth doing first
1. Log `eye->ambientRed` per frame for the first ~12 frames of mc2_10 —
   closes the 63-provenance gap in §1.
2. Confirm the user's trucks sit inside the radius of an actual
   `TG_LIGHT_POINT`/`SPOT` fixture. If they do not, the pools slice is wrong
   and the answer is closer to "stock-correct, ambient is simply low for
   untagged props" — which is a tuning question, not a bug.

---

## 5. DEAD — DO NOT RE-OPEN

Everything in the prior kill-list, plus, newly closed by this lane:

- **The frame 0→1 split** — explained: `ambientRed` 192→63 via
  `camera.cpp:423` load default vs `updateDaylight`. No tree, no order, no
  sun-window involvement.
- **"172 recipes lose the bright value at the invalidation"** — correct
  behaviour, not a defect. Do not restore 0.7529.
- **Bright-is-correct hypothesis (mission branch 3)** — refuted. 0.7529 is the
  day ambient leaking into a night mission through a boot-time bake.
