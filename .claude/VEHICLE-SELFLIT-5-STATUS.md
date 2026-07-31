# VEHICLE-SELFLIT-5 — STATUS

**Headline: `MC2_LEGACY_LIT_MC2_FRAME` does NOT fix the reported defect. It
changes ZERO pixels on the objects in question. Do NOT default it ON.**

**Second headline: the objects the user is pointing at in the mc2_10 compounds
are STATIC PROPS, not `GVAppearance` ground vehicles — proven this lane with a
live white-probe, no build required.** This independently confirms
VEHICLE-BAKED-TRUCK-3 §3 item 4, and it means the premise carried by lanes 1, 2,
4 and by this lane's own tasking ("vehicles are the odd one out, the only unit
class still on the legacy CPU path in `gvactor.cpp`") **does not describe the
pixels being complained about.**

- Worktree `A:/Games/mc2-main`, branch `claude/nifty-mendeleev`, HEAD `ce68d5e4`
- **No repo-content commits. No build. No deploy. No push.** Env-only probes.
- Evidence dir: `scratch/vehselflit5/`

---

## 1. Build attribution (the thing prior lanes got wrong)

Prior lanes' evidence was unattributable because it came from dirty-tree exes.
This lane used **one clean, self-consistent, verified install** and made no
build at all:

| item | value |
|---|---|
| exe | `A:/Games/mc2-opengl/mc2-win64-v0.4c/mc2.exe` |
| embedded sha | `608dea4db465` |
| manifest `src_commit` | `608dea4db465`, **`dirty=false`** |
| md5 before all runs | `c8618c1287a640692ce3325f2f6a5603` |
| md5 after all runs | `c8618c1287a640692ce3325f2f6a5603` (**identical**) |

`608dea4d` contains `b01a7a96` (the `MC2_LEGACY_LIT_MC2_FRAME` fix) and
`5213d257` (light-pool max) — verified by `git merge-base --is-ancestor`. It is
only 15 commits behind HEAD.

**Why no build was made:** the worktree is `safe_to_touch=false` —
`GameOS/gameos/gos_terrain_indirect.cpp` is dirty with **live, coherent,
in-progress work owned by CEMENT-UNIFORM-2** (a real fix, not broken source).
Building here would stamp `MC2_BUILD_GIT_DIRTY=1` and produce exactly the
unattributable evidence that invalidated prior lanes. Per the standing rule the
lane **waited** instead. The 0.4c install made the decisive test possible
without one.

Rejected deploy target for the record: **`0.4d-rc1` is `FINGERPRINT-MISMATCH`
and dirty** — exe embeds `98ba4155`, manifest says `6aaba337-dirty`, neither
matches HEAD. Any evidence captured against it is worthless. This is the
"one install's manifest sha is wrong" case named in the tasking; it is 0.4d-rc1.

## 2. The decisive A/B — gate does nothing to the objects in question

Harness `scripts/run_visual_capture.py` (chosen deliberately: it does
`env = os.environ.copy()` with **no allowlist filter**, unlike `run_smoke.py`).
Poses: `scratch/vehselflit5/mc2_10_vehlit.json` (compound close/mid/wide +
tower area). OFF → `scratch/vehselflit5/off/`, ON → `scratch/vehselflit5/on/`.

**The gate demonstrably armed** — 3 of 4 pose hashes changed. This kills the
"the probe proved nothing / it was never really on" worry:

| pose | OFF hash | ON hash | changed px | mean signed dLum |
|---|---|---|---|---|
| compound_close | `3887263b…` | `d007d57f…` | 125,050 (1.51%) | — |
| compound_mid | `54851611…` | `c26adf4c…` | 613,326 (7.39%) | **+22.77** |
| compound_wide | `1b4f8847…` | `cf578149…` | 956,957 (11.54%) | **+24.85** |
| tower_area_204611 | `d3129321…` | `d3129321…` | **0 (control)** | — |

**But on the vehicle itself, the gate changes nothing at all.** Measured over a
generous 70x100 px box around the parked truck in `compound_wide`:

```
truck box OFF mean RGB [116.7 115.0 89.7]  lum 107.15
truck box ON  mean RGB [116.7 115.0 89.7]  lum 107.15
changed px in truck box: 0 of 7000
```

Zero. Not "small" — **zero**.

**What the gate actually changes** (`diffmask_compound_wide.png`,
`diffmask_compound_mid.png` — changed pixels painted red over the OFF frame):
a single large elliptical **ground light pool** in the upper-left, plus a few
specks on one tower. Nothing else. It brightens that pool by ~+25 luminance.

### Verdict on the gate

- It does **not** fix vehicles/props. It never touched them.
- Its real effect is on a large ground light-pool primitive, which it makes
  **brighter**. That is an unreviewed visual change on a surface nobody
  evaluated, i.e. a **possible regression**.
- **Recommendation: leave `MC2_LEGACY_LIT_MC2_FRAME` DEFAULT-OFF.** Do not
  promote. The coordinate-frame reasoning in `b01a7a96` may well be
  *correct in the abstract*, but its only observable consumer here is that
  ground pool, and its effect on the reported defect is nil. Promoting it would
  ship a brightness change with no benefit to the complaint.

## 3. What the complained-about objects actually are — STATIC PROPS

Run `MC2_GPU_PROPS_DEBUG_MODE=3` (`gos_static_prop_batcher.cpp:5901` →
`u_debugAddrMode=3` → `static_prop.frag:385` writes solid white). All 4 pose
hashes changed vs baseline, so the probe is confirmed live (lane 3's trap #3
respected). Output: `scratch/vehselflit5/propprobe/`.

`propprobe/mc2_10_compound_close.png` turns **white**: every building, every
wall segment, every turret emplacement, the radar mast, the fence, the storage
domes — **and the parked brown truck, and the small blue crate.**

So in these frames:
- The compound "vehicles" **are static props**, drawn by `static_prop.vert/frag`
  through the GPU static-prop batcher.
- There are **no `GVAppearance` ground vehicles in frame at all.**
- Therefore `gos_tex_vertex_lighted` (the `gvactor` legacy path, and the whole
  subject of `b01a7a96`) is **not what paints them** — which is exactly why the
  gate moved zero pixels on the truck.

This is a clean, direct, no-build confirmation of the hypothesis
VEHICLE-BAKED-TRUCK-3 raised but could not close.

### No magenta-probe methodology traps were hit
No shader was edited at all (both traps are about shader edits): the probe is a
pre-existing engine debug mode driven purely by env. Consequently there was
nothing to restore, `shaders/spv/` was irrelevant, and `git status -- shaders/`
is clean.

## 4. The honest remaining gap — and why it is blocked

The user's report is about the **mc2_10 intro**, where real mobile units *are*
on screen. **My captures contain no real ground-vehicle units**, because the
0.4c exe predates `MC2_FORCE_ALL_VISIBLE` (`ce68d5e4`, landed by
FORCE-ALL-VISIBLE-1 during this lane).

So this lane can state with confidence:
- the gate is not the fix, and
- the compound props the user can see are static props,

but it **cannot yet** answer "do genuine `GVAppearance` units render darker than
mechs under the same light", because it could not get one into frame.

Closing that needs `MC2_FORCE_ALL_VISIBLE=1` → needs a build at HEAD → **blocked
on CEMENT-UNIFORM-2's dirty `gos_terrain_indirect.cpp`.** The moment that lane
commits, this is a warm build in `mc2-main` plus two capture runs.

Also unresolved and **not** claimed as done: **day check and tier1 5/5 were not
run** (they require the same blocked build to be meaningful; running them
against a known-mismatched install would repeat the prior lanes' error).

## 5. Recurrence-proofing found but NOT applied (lane conflict)

`MC2_LEGACY_LIT_MC2_FRAME` is `verdict=GAPS`, flag **`ENV_DROP_RISK`**: read by
`mclib/txmmgr.cpp:2199` but **absent from the `run_smoke.py` passthrough
allowlist**. Any A/B run through `run_smoke.py` silently executes the **gate-OFF**
path. This is almost certainly the mechanism behind at least one prior
"verified via smoke" claim.

**Not fixed here on purpose:** `check_conflict` reports `scripts/run_smoke.py`
claimed by **two live lanes** (CEMENT-ALPHA-LAND-1, FORCE-ALL-VISIBLE-1).
Editing it would be the exact double-implementation collision the lane registry
exists to prevent. Handing off instead:

> Add `MC2_LEGACY_LIT_MC2_FRAME` to the `run_smoke.py` passthrough allowlist and
> to the `check-env-registry.sh` ALLOWLIST. Allowlisting only enables
> passthrough when explicitly set — it does not change the default-OFF.

## 6. Next steps, in priority order

1. **Wait for CEMENT-UNIFORM-2 to commit**, then one clean-fingerprint build at
   HEAD (`--require-clean`, md5-verify, record sha).
2. Re-run the compound + lance poses with **`MC2_FORCE_ALL_VISIBLE=1`** so real
   GV units are guaranteed in frame. *Only then* is the user's actual comparison
   testable. Get a mech and a GV unit in the SAME frame.
3. If GV units then look correct → the defect is **within the static-prop
   class** (why does a prop truck sitting inside a lit ground pool read darker
   than the building beside it?), and this becomes a static-prop lighting slice,
   **not a vehicle slice**. Coordinate with VISIBILITY-SEMANTICS-1, which owns
   the self-lit/emission predicate.
4. If GV units look dark → resume the structural GPU-batcher-parity idea, but
   attribute first against the **HDR scene FBO**, never the backbuffer.
5. Retire or re-scope `MC2_LEGACY_LIT_MC2_FRAME`. It is a fix in search of a
   consumer; its only observable effect is a ground light pool.

## 7. Lane coordination

- Repo-content edits: **none**. Only `.claude/VEHICLE-SELFLIT-5-STATUS.md` and
  untracked `scratch/vehselflit5/`.
- No build, no deploy, no push, no `--amend`. Harness cursor guard untouched.
- Did not edit `scripts/run_smoke.py` (2 live claims), `mclib/gvactor.cpp`
  (VISIBILITY-SEMANTICS-1), or the shared 0.4c install.
