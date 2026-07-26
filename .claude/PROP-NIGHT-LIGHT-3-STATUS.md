# PROP-NIGHT-LIGHT-3 — status (2026-07-25)

Lane: PROP-NIGHT-LIGHT-3. Worktree `A:/Games/mc2-main`, branch
`claude/nifty-mendeleev`. Base HEAD `ffc0f10a` → landed `d99bd798`.

**VERDICT: 1 of 3 objectives LANDED. The other two are BLOCKED on evidence that
cannot be produced attributably right now, and were deliberately not faked.**

| Objective | Outcome |
|---|---|
| Land `PROP-ALPHA-SHADOW-CLASS-1` | ✅ **LANDED** `d99bd798` — and a real defect in the inherited design was caught and corrected |
| Flip `MC2_PROP_SHADOW_RECEIVE` / `MC2_SSAO` default-ON | ❌ **NOT DONE** — gated on fence evidence; captures blocked (§3) |
| Defect (a): props dark at night | 🔬 **ROOT-CAUSE CANDIDATE FOUND, brief's premise FALSIFIED** (§4) |
| `MC2_STATIC_PROP_NIGHT_TAGS` flip | ❌ **RECOMMEND AGAINST** — would widen the defect, not fix it (§5) |

## 1. META REVIEW — inherited premises, corrected

Checked against source, not status docs. Four corrections:

| # | Brief / inherited claim | Reality |
|---|---|---|
| C1 | "tree CLEAN as of `1779bf1d`" | HEAD was **`ffc0f10a`** (2 commits later). Tree was clean of *tracked* modifications at session start. |
| C2 | PROP-SHADOW-ENABLE-2 blocker **B2** — CEMENT-UNIFORM-2 holds uncommitted `gos_terrain_indirect.cpp` | **RESOLVED.** That work was committed as `1779bf1d`. `git status --untracked-files=no` was empty. |
| C3 | PROP-SHADOW-ENABLE-2 blocker **B1** — `static_prop.frag` claimed by live EDGE-LIGHT-EMISSION-1 | **Stale claim.** That lane left no commit touching `static_prop.frag` and no status doc. Claim released by me implicitly; file was landed on cleanly. |
| C4 | Defect (a): "`base_light` comes from a per-leaf constant `child->GetARGBHighlight()`" | **FALSIFIED — this is the wrong function entirely.** See §4. |

**A methodological correction I must flag against myself:** mid-session I
inferred "lane pid 22264 is dead ⇒ those lanes are stale" from `tasklist`.
**That inference was unsound** — my *own* fresh `register_lane` call also
returned `pid: 22264`, so that field records the **MCP server's** pid, not the
agent's. It is useless for liveness. The B1/B2 conclusions above (C2, C3) do
**not** rest on it — they rest on git state (committed / no commits), which is
sound. But the reasoning step was wrong and is corrected here rather than left
in the record.

## 2. LANDED — `PROP-ALPHA-SHADOW-CLASS-1` (`d99bd798`)

3 files, +65/−3: `render_contract.hglsl`, `static_prop.frag`,
`shadow_screen.frag`. Emits a second in-band GBuffer1.a value for alpha-test
LATTICE props (fence/railing/grating), so the `MC2_PROP_SHADOW_RECEIVE` opt-in
can exclude them — they'd otherwise self-shadow as solid slabs because the
shadow depth pass has no alpha discard. Discriminator is the existing
`ALPHA_TEST_BIT` in `u_materialFlags`, already set per packet at
`gos_static_prop_batcher.cpp:2666`. No new uniform, no SSBO field, no batcher
change, no bit-packing, no threshold move.

### ★ The inherited design was WRONG on the value, and it mattered

PROP-SHADOW-ENABLE-2 specified **0.375** and asserted the change was a
"provable no-op". **It was not.** Its consumer audit only grepped the `rc_*`
helper names and the census script, and therefore missed a **continuous**
reader of the same channel:

```glsl
// shaders/shoreline.frag:22  AND  shaders/vulkan/shoreline.frag:102
float myAlpha = normalData.a;
bool isWater = (myAlpha > 0.15 && myAlpha < 0.35);
```

The existing 0.25 prop class sits **inside** that water band. **0.375 sits
outside it** — so the inherited design would have silently pulled every
alpha-test prop out of the shoreline pass and changed shipped pixels, while
being documented and committed as a no-op. That is the exact failure class the
"provable no-op" framing was meant to prevent.

**Landed value is `0.1875` (= 3/16, exact in fp16).** It sits inside the water
band exactly as 0.25 does, so every continuous reader is bit-for-bit
unaffected. It clears the mech threshold (`a <= 0.1`) by 0.0875 and sits 0.0625
from 0.25 — both margins ~3 orders of magnitude above the fp16 ulp there.
Reader predicate `rc_isAlphaTestProp` carves sub-band `(0.1, 0.22]`.

**Note the latent pre-existing bug this exposed:** static props at 0.25 are
*already* misclassified as water by both shoreline shaders. Not touched by this
lane (out of scope, and fixing it moves shipped pixels), but it is real and
should be raised separately.

### Verification actually performed

- `scripts/check-render-contract-gbuffer1.sh` → **PASSED** (a new `rc_*` helper
  trips neither of its two rules; no census edit needed — inherited claim holds).
- `scripts/validate_shaders.py` (glslangValidator) → **`[PASS] shaders\static_prop.frag`**,
  **`[PASS] shaders\shadow_screen.frag`**. The suite's other failures
  (`water_mvp.frag`, `postprocess_blur.comp`, `shaders/vulkan/*`) are
  pre-existing `#version`-prefix / missing-`#define` variant issues in files
  this lane never touched.
- Consumer sweep for every GBuffer1.a reader across `shaders/**` — the two
  `rc_*` predicates, the two VK hand-duplicated copies, and the two shoreline
  continuous reads. All enumerated; all confirmed unaffected at 0.1875.
- **VK duplicates flagged as a standing hazard:** `shaders/vulkan/screen_shadow_csm.frag:12`
  and `screen_shadow_static.frag:7` **hand-copy** the band predicates instead of
  including `render_contract.hglsl`. They match 0.1875 correctly today by luck of
  the band, and VK has no RECEIVE opt-in, so VK is unaffected — but the contract
  is duplicated in 3 places and will drift.
- **NOT run:** `scripts/check-contracts.sh` (65/65). It hung, and it is itself
  **being modified right now** by a concurrent lane (§3) — its result would be
  meaningless. Not claimed as passing.

## 3. BLOCKER — a live concurrent lane owns the harness

`VERIFY-INTEGRITY-3` is **actively writing this same worktree**, mid-edit, right
now. Confirmed by direct observation: `git status --untracked-files=no` was
*empty* at session start and grew 9 modified files during this session.

Its uncommitted files include **`scripts/run_smoke.py`, `scripts/game_auto.py`,
`scripts/capture_baseline.py`, `scripts/check-contracts.sh`** — precisely the
harness required for the capture matrix — plus **staged deletions** of
`tests/visual/bookmarks/mc2_{03,10,17,24}.json`, i.e. the bookmark files for
three of the four maps the verification matrix names.

Consequences, stated plainly:
- Any build embeds `MC2_BUILD_GIT_DIRTY 1` → every capture unattributable.
- `deploy --require-clean` would fail by design.
- Captures would run through **half-written harness code** and **deleted
  bookmarks**.

So **no build, no deploy, no captures were taken.** Producing the day/night ×
4-map matrix under these conditions would have yielded evidence that looks
authoritative and means nothing. Per the mission's own build-attribution rule,
this lane declined.

**Commit hygiene under a shared index:** the git index is shared with that lane
and already held its staged deletions. A plain `git commit` would have scooped
them. I used a **pathspec-limited `git commit --only <3 shader paths>`**;
verified afterwards that the commit contains exactly 3 files and the other
lane's staged state is intact and untouched.

## 4. Defect (a) — the brief's stated mechanism is FALSIFIED; a better candidate found

**`GetARGBHighlight()` is not the light seed.** `mclib/tgl.h:836` —
`DWORD aRGBHighlight; //Color to add to vertices to make building stand out.`
It is the **selection-highlight** colour, init `0x00000000`, set only via
`SetARGBHighlight()`, consumed in the *fragment* shader as `v_highlightColor`.
It never enters `base_light`. Chasing it further is wasted effort.

**What `base_light` actually is:** the per-vertex attribute `a_aRGBLight`
(`shaders/static_prop.vert:31`), sourced from `TG_TypeVertex::aRGBLight` —
authored per-**type** `.ase` vertex colour, uploaded once at
`finalizeGeometry()`. Constant per prop type; not a runtime terrain sample.

**Also falsified:** static props never see night light *pools* at all.
`static_prop.vert:565-600` does not call `calc_light()`; it open-codes a
replacement that accumulates only `TG_LIGHT_AMBIENT` rows plus the *first*
`TG_LIGHT_INFINITE` row, and explicitly discards POINT/SPOT
(`static_prop.vert:582-583`: *"Point/spot lights are omitted (negligible for
buildings)"*). This is symmetric across all props, so it cannot produce the
building-vs-truck differential — but it does explain why every prior
"light-pool" theory failed. There is no radius/extent/attenuation term to lose,
and no per-object light clamp that big props win (`txmmgr.cpp:2269-2395` walks
a global list in index order, `break`s at 32, no sorting, no size term).

### ★ Root-cause candidate #1 — Tree-population props are handed NO directional light

Verified verbatim in source. Buildings and small scenery props take **different
appearance classes with asymmetric light setup**:

`BldgAppearance::update` (`mclib/bdactor.cpp:3638`) writes the terrain-light
sample into **slot 0 — the SUN** (`camera.cpp:487` `worldLights[0]->init(TG_LIGHT_INFINITE)`),
and leaves it active:
```cpp
eye->setLightColor(0, lightRGB);
eye->setLightIntensity(0, 1.0);
```

`TreeAppearance::update` (`mclib/bdactor.cpp:6706`, `:6719-6720`) writes it into
**slot 1 — AMBIENT** (`camera.cpp:514`), and then **deactivates the sun**:
```cpp
eye->setLightColor(1, lightRGB);
eye->setLightIntensity(1, 1.0);
...
TG_LightPtr light = eye->getWorldLight(0);
light->active = false;                       // <-- kills the directional row
treeShape->SetLightList(eye->getWorldLights(), eye->getNumLights());
```

So a Tree-population prop's cached light row contains **no directional light at
all** → in `static_prop.vert`, `sunFound == false`, `sunColor == 0`, and
`lit = base_light + ambientTerm` where `ambientTerm` has been overwritten with
that prop's own dark night terrain sample. A Building-population prop at the
same spot keeps both a directional term and the camera's global ambient. That
is exactly "same location, building lit, small prop near-black".

`GpuStaticPropPopulation` is `Building / Tree / Generic / Legacy`
(`gos_static_prop_batcher.h:128-133`); `TreeAppearanceType` is instantiated for
`TREED_TYPE` (`mclib/apprtype.cpp:363-366`). In MC2 data, crates/barrels/parked
scenery vehicles are commonly TREED-class scenery, not BLDG.

**Candidate #2 (same family, different site):** `GenericAppearance` calls
`genShape->SetLightList(NULL, 0)` (`mclib/genactor.cpp:1238`) → `numLights == 0`
→ `lit = base_light` alone, i.e. raw authored vertex colour, no lighting at all.

**THE ONE LOAD-BEARING UNKNOWN:** which appearance class the specific mc2_10
truck/crate instantiates. Not resolvable from source alone; it decides between
#1 and #2. **It needs a run, and a run needs a clean tree (§3).**

**Cheapest discriminating test for the successor — no code change required:**
`MC2_LIGHTING_DEBUG_VIEW=sun` (resolver `gameos_graphics.cpp:7078`, static-prop
consumer `gos_static_prop_batcher.cpp:6535`). Under #1 the parked truck renders
**black** while the adjacent building shows a nonzero sun term — one frame
separates #1/#2 from an authored-vertex-colour explanation. `=lightcount` /
`=lightindex` then separate #1 (row present, no directional entry) from #2
(`numLights == 0`).

## 5. `MC2_STATIC_PROP_NIGHT_TAGS` — RECOMMEND AGAINST flipping

Env var `MC2_STATIC_PROP_NIGHT_TAGS`, **default 0 = OFF**, parsed
`gos_static_prop_batcher.cpp:790-796` (clamped 0..2); companion
`MC2_STATIC_PROP_NIGHT_TAGS_SCALE` at `:797-807` (default 1.0, or 4.0 in mode 2).
The `false, 0.0` the brief cites is `shaders/static_prop.vert:488` —
`get_base_light(perVertexARGB, false, 0.0, ...)`, the hardcoded day-state stub.
Scale 0 is provably the pre-fix render, so the user does still see the defect.

"Night vertex tags" are stock magic vertex colours decoded in
`shaders/include/lighting.hglsl:70-168`: `0xffff00ff` hot pink = lit windows,
`0xffffff00` hot yellow = outside building lights, `0xff00ff00` hot green =
building base lights — all three night-only branches. Only geometry authored
with those exact colours carries them: runway/beacon fixtures
(`data/tgl/{runway,red,green,blue}beaconlight.ase`), building `LitWin_*` nodes,
building base lights.

**A truck or crate carries none of these.** Its seed falls into the generic
`(start_v_light & 0x00ffffff) != 0` arm, which is night-state-independent.
Turning the gate ON would relight **buildings** further — i.e. it would
**widen** the very building-vs-prop contrast the user is complaining about. It
is plausibly the right fix for the *separate* "runway/beacon lights not on"
defect, but it must not be flipped as a fix for (a), and it needs the night A/B
at airbase range **and** overview altitude (streak check) that the brief
specifies — which is blocked by §3.

## 6. Recommended successor sequence

1. **Wait for VERIFY-INTEGRITY-3 to commit/release** the harness scripts and the
   4 bookmark files. Re-run `preflight` until tracked-clean.
2. Clean build → `deploy --require-clean` → md5 verify → record sha.
3. **Resolve §4's single unknown first** with `MC2_LIGHTING_DEBUG_VIEW=sun` at
   the mc2_10 night pose. It is one frame and it decides the whole (a) fix.
   Fix is then a small symmetry repair in `TreeAppearance::update`
   (`bdactor.cpp:6706/6719`) or `GenericAppearance` (`genactor.cpp:1238`).
4. **Fence evidence** for the shadow flip, on mc2_01 day: fences casting onto
   ground = YES, fences self-shadowing as slabs = NO. Cheap, and it is the gate
   for step 5.
5. Only then the default-ON flip, **split**: `MC2_PROP_SHADOW_RECEIVE` (cheap,
   bounded by 0.25–0.73 % prop pixel coverage) and `MC2_SSAO` (full-screen
   half-res pass, **not** bounded by prop coverage) are different perf
   propositions. SSAO needs a free-timestep Tracy night A/B before anyone
   defaults it — the light-pool cap raise already cost ~34 % at the busiest
   night pose.
6. Consider separately: the shoreline/prop 0.25 misclassification (§2) and the
   3-way duplication of the band contract into `shaders/vulkan/*` (§2).

## 7. Lane hygiene

- Commit `d99bd798`, 3 shader files only, pathspec-limited so the concurrent
  lane's staged work was not scooped (verified after the fact).
- No build, no deploy, no smoke, no captures, no push, no `--amend`.
- **DEFECT IN MY OWN COMMIT, flagged not hidden:** the subject line reads
  `@ feat(shadow): ...` — a stray `@` from a PowerShell here-string used in a
  Bash heredoc. Cosmetic, breaks conventional-commit parsing. **Not amended,
  because `--amend` is a standing prohibition in this lane's rules.** Fixing it
  requires an explicit user decision to permit a history rewrite.
