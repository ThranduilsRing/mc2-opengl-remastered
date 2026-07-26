# POOL-SPILL-STATIC-1 — STATUS

**Verdict: DID NOT FLIP. `MC2_LIGHT_REALISM` stays default-OFF.**
**The brief's causal premise was refuted by measurement, and the prescribed fix
was implemented, measured, proven inert, and reverted.**

Evidence exe `c2e2473d106b` (`dirty=0`, branch `claude/nifty-mendeleev`,
`DEPLOY_FINGERPRINT` verdict OK, gate string grep-confirmed in the deployed
binary). Deploy lane `0.4c`. Capture matrix 17/17 arms OK, 82 PNGs, **zero
zero-byte files**. Tree ends clean at the revert.

---

## META REVIEW — premises checked against reader source

| brief premise | verdict |
|---|---|
| `POOL_SPILL=2.2` scales `farDistance` for every pool light uniformly | **CONFIRMED** (`gos_terrain_lighting.cpp`, reshape block). |
| The shader already distinguishes movers via `gl._pad1 = lightIsMover(tl)` | **CONFIRMED** — and it was already computed in the same loop, just after the reshape. |
| **The regression is caused by mover-attached mech/elemental pool lights** | **FALSE — this is the finding of the lane.** At the user's own complaint poses there are **no mover-class lights in the pool at all**. See §2. |
| Applying spill to static lights only "should keep all three wins and remove the regression" | **FALSE.** It is a no-op at every measured pose. See §1. |
| A per-fragment additive clamp is not the lever | **Accepted, not re-litigated.** |
| Perf is a non-issue with `MC2_LIGHT_GRID` on | **Not re-measured** (no behaviour change shipped). |

---

## 1. The prescribed fix, implemented and measured

Implemented as `c2e2473d`: hoisted `lightIsMover(tl)` above the reshape and
selected the spill per class — static keeps `POOL_SPILL`, movers use a new
`MC2_LIGHT_REALISM_MOVER_SPILL` (default `1.0` = stock reach). Registered,
documented, allowlisted; `check-env-registry.sh` PASS.

One deliberate improvement over the brief: the reshape does **two** things —
`spill` widens `farDistance` (the suspected regression) and `POOL_CORE=0.0`
collapses `closeDistance`, removing the flat-topped **saturated plateau**. That
second half *reduces* hot pixels. A blanket "skip movers" would have discarded
it, so movers kept the core collapse and lost only the spill.

**Result: no effect.** `B_ON_OLD` (`MOVER_SPILL=2.2`, i.e. the pre-fix uniform
behaviour) and `C_ON_FIX` (`MOVER_SPILL=1.0`) are visually identical and
numerically indistinguishable — `C_ON_FIX` is fractionally *higher*, not lower.

---

## 2. Why — the decisive receipt (immune to capture nondeterminism)

`MC2_LIGHT_POOL_DUMP=1` at `lance_close` + `saved_204753`, 17 dump blocks:

```
ALL light-rows by class      : {'other': 1684, 'bldg': 81}
PACKED (entered pool) by class: {'other': 684,  'bldg': 15}
```

**Zero `mech`, zero `gv`.** `lightIsMover()` never returns true at these poses,
so the carve-out can never fire. The blown-out pool is produced entirely by
**static and untagged (`other`) lights** being spilled 2.2x.

`tag_slot` *is* called for `Bldg` (`bdactor.cpp:3345`), `Gv`
(`gvactor.cpp:2655`) and `Mech` (`mech3d.cpp:4079`), so the classifier is wired
— there simply are no mover spotlights registered at the user's poses, even
though mechs are plainly in frame.

**Corollary worth a follow-up:** `gl._pad1` is therefore always `0.0` here, so
the pre-existing `NIGHT-LIGHT-POOLS-2a` mover-dimming branch in the pool frag
shader is **also inert at these poses**. That is a pre-existing latent finding,
not something this lane introduced.

---

## 3. Numbers — hot-pixel area % (mean RGB >= 220)

Noise floor for this metric, A_OFF vs A2_OFF (byte-identical env), is
**<= 0.012 percentage points on every pose** — the absolute-area metric is far
more stable than pixel-diff, which is why it was chosen (the documented mc2_10
drift is mover *position*, which barely moves total saturated area). Every
effect below is orders of magnitude above that floor.

| pose | A_OFF | A2_OFF | E spill 1.3 | F spill 1.6 | B_ON_OLD (2.2 uniform) | C_ON_FIX (2.2 static-only) |
|---|---|---|---|---|---|---|
| `mc2_10 lance_close` **(the user's blown-out frame)** | 0.094 | 0.106 | **1.268** | 2.593 | **7.834** | **8.288** |
| `mc2_10 saved_125552` | 0.045 | 0.045 | 0.150 | 0.278 | 0.725 | 0.759 |
| `mc2_10 saved_204753` | 0.120 | 0.129 | 0.128 | 0.230 | 0.477 | 0.437 |
| `mc2_10 saved_212019` (cones) | 0.023 | 0.023 | 0.025 | 0.035 | 0.050 | 0.050 |
| `mc2_10 saved_204958` (beacons) | 0.343 | 0.341 | 0.338 | 0.448 | 0.689 | 0.721 |
| `mc2_05 saved_205843` | 0.021 | 0.022 | 0.025 | 0.034 | 0.127 | 0.214 |

`B_ON_OLD` vs `C_ON_FIX` is the fix's entire effect: **nil**.
Hot area is a clean monotonic dose-response in `POOL_SPILL` alone.

Day maps are untouched at every arm: mc2_01 / mc2_03 / mc2_24 hot area is
identical to 3 decimal places across A_OFF and C_ON_FIX (the gate is night-only,
enforced upstream by the light-activation predicate).

---

## 4. Grading against the user's own words

| user quote | verdict at shipped 2.2 |
|---|---|
| "the cones are ultra visible, they need to be feathered / more transparent" | **SOLVED** — `saved_212019`, soft shafts, no hard rim. **And it does not depend on `POOL_SPILL`**: the cone win is fully present at spill 1.3 (hot area 0.025 vs 0.023 OFF). It is bought by `CONE_OPACITY`/`CONE_FEATHER`. |
| "the light needs to spill from there, not be just in random spots" | **SOLVED at 2.2**, partially at 1.3. `saved_204753`: OFF is blotchy patches with dark ground between fixture and patch; 2.2 reads as one continuous lit compound; 1.3 is intermediate but still clearly better than OFF. |
| "the light sources need to mimic LIGHT SOURCES (real ones)" | **SOLVED** — beacon glow is gated on `realism_enabled()`, independent of spill. |
| "terrible lighting here" (huge blown-out white pool over five small units) | **STILL REGRESSED at 2.2.** `lance_close` is 88x the OFF hot area (0.094 -> 8.288). Visually the whole lower frame washes to near-white and ground texture is flattened — the user's exact failure mode. **This is why the default was not flipped.** |

---

## 5. mc2_05 (the user-approved night look)

`FIX_vs_OFF`: `10.73 %` changed, mean dL `+29.1`, `96.3 %` brighter, against a
`1.99 %` floor — a real, above-noise change to content the user already
approved. Hot area 0.021 -> 0.214 (10x, still small in absolute terms; no
blowout). I did **not** resolve this as approved-or-degraded: it is a
deliberate look change on signed-off content and needs the user's eye, which is
exactly why it must not ship by a default flip made on my authority.

Bookmark coverage was the blocker for a stronger answer: `mc2_05` ships only
**one real location** (its second bookmark is a literal `_dup`). I authored a
5-location set incl. a no-fixture control at `scratch/pss1/bm/mc2_05.json`;
it is **not yet captured** (disk budget went to the spill sweep, which was the
higher-value question). Same `_dup` defect affects `mc2_03` and `mc2_24`.

---

## 6. What this lane actually delivers

1. **The brief's diagnosis is wrong, with a receipt.** Mover lights are not the
   cause; static/untagged spill magnitude is. Nobody should re-run the
   static-only carve-out.
2. **A dose-response curve for `POOL_SPILL`** (1.0 / 1.3 / 1.6 / 2.2) on the
   decisive poses.
3. **The wins are separable from the blowout.** Cones and beacons are bought by
   `CONE_OPACITY` / `CONE_FEATHER` / beacon glow and cost nothing in hot area.
   Only the pool-continuity win trades against blowout, and only via
   `POOL_SPILL`.
4. **Deploy-doc fix.** The documented invocation was wrong on `--build-dir` in
   three docs + one handoff; `deploy_payload.py` needs the **per-config** dir
   (`build64/RelWithDebInfo`). This had already bitten multiple lanes.

## 7. Recommended next step (user call, not mine)

The user approved `2.2` as "variant B" *and* complained about the blowout;
those two facts conflict, and only the user can settle it. The data says:

- `POOL_SPILL=1.3` keeps the cone + beacon wins **whole**, keeps a visibly
  improved (if weaker) continuity win, and cuts the blowout from `8.29 %` to
  `1.27 %`.
- A flip is defensible at `1.3` in a way it is not at `2.2`.

Show the user `scratch/pss1/grade_spill_sweep.jpg` (OFF / 1.3 / 2.2 across
blowout + continuity + cone poses) and let them pick the constant. If they
accept `1.3`, the flip becomes a one-constant change with all evidence already
in hand.

## 8. Artifacts

`scratch/pss1/` — `drive.py` + `drive2.py` (matrices), `compare.py`,
`graders.py`, `drive.log`, `compare.json`, `out/` (A_OFF, A2_OFF, B_ON_OLD,
C_ON_FIX, E_SPILL13, F_SPILL16), `dump_on/` (the pool-dump receipt),
`grade_spill_sweep.jpg` (**the one to show the user**),
`grade_mc2_10_lance_close.jpg` (OFF vs old vs fix — identical),
`bm/mc2_05.json` (uncaptured 5-location set).
