# REVIEW-EXHAUSTIVE-1 — lanes/efforts log + reader-source gate truth

Lane `REVIEW-EXHAUSTIVE-1`, worktree `A:/Games/mc2-main`, branch
`claude/nifty-mendeleev`. **No engine source touched. Nothing pushed.**

---

## 1. META REVIEW — the brief's premises, checked against source

The brief asked for an exhaustive screenshot pass, a lanes log, a truthful gate
table, and a survivable generator. Four of its premises were already overtaken
before this lane started. Verified, not assumed:

| Brief premise | Reality at lane start | Source |
|---|---|---|
| "compose exhaustive captures across 5 maps, day+night, ≥3 locations" | **Already done** by `REVIEW-ALLPOSES-1` (19 poses / 5 maps / 6 top-downs) from a clean-fingerprint exe | `.claude/REVIEW-ALLPOSES-1-STATUS.md` |
| "a prior audit found 48 orphaned PNGs" | **Already repaired.** `--check` reports 130 on disk / 130 referenced / **0 orphans / 0 missing** | `build_review_pack.py --check` |
| "a tracked manifest + generator already exists — extend and use it" | True, and it **already reproduced the live page exactly** (15/15 section ids, 128/128 img srcs, 15/15 headings, in order) | verified by structural diff |
| user bookmark `mc2_24_saved_191157` / `192916` | The user's mc2_24 save is **`192316`** | ALLPOSES bookmark inventory |
| "the four bookmark fixtures were deleted as fraudulent" | **Confirmed** (`eff3a32b`) — and honest replacements were authored by ALLPOSES/VIS-SEMANTICS-FINISH-2 | git |

So the genuinely-missing work was (2) the lanes log and (3) gate defaults for the
gates the registry cannot describe. That is what this lane did.

## 2. Why NO new captures were taken (the honest answer)

The brief's own bookend asks whether a gate's defect would be visible on a
default launch, and requires a **clean-fingerprint build deployed to the leased
path**. Neither was obtainable:

- `preflight` → `safe_to_touch=false`; `GameOS/gameos/gos_terrain_indirect.cpp`
  dirty, claimed by three other live lanes. Any build now embeds
  `MC2_BUILD_GIT_DIRTY=1` and its captures are **unattributable**.
- `deploy_status 0.4c` → a **fresh foreign smoke lease** (another session,
  `missions:mc2_05`, age 12s) plus `verdict=FINGERPRINT-MISMATCH`.

Fabricating a capture pass under those conditions is exactly the failure mode
this session spent lanes repairing. **No images were added or replaced.** Every
image on the page remains ALLPOSES's attributable set.

**Attributable exe behind the page's evidence: `c3804cfebb6b`, `MC2_BUILD_GIT_DIRTY 0`**
(ALLPOSES-attested: build exit 0, `--require-clean` deploy, manifest
`src_commit c3804cfebb6b`, artifact md5 `004ceea6f263662938303e5cfab9b00f`,
tree re-checked clean mid-capture). Note `0.4c` has since been re-deployed to
`06ffbcf4b09f` by `CEMENT-ATLAS-CRASH-1`, so that install can no longer
re-verify the older set.

## 3. What shipped

### `scripts/build_review_pack.py` — reader-source default resolution
The registry covers only part of the pack: **18 gates named on the page have no
`RendererFeatureRegistry.h` entry**, so a registry-only table could say nothing
about their real defaults. Added `resolve_reader_default()`, which classifies
the C++ `getenv` idiom **at render time** and publishes the matched source line
as the citation. Results (15 resolved, 1 unresolved, 1 ghost):

- ON: `MC2_VIS_SEMANTICS`, `MC2_LETTERBOX_OPAQUE`, `MC2_CEMENT_MISSION_LIGHT`,
  `MC2_LIGHT_CULL_RADIUS`, `MC2_SHADOW_ALT_SCALE`, `MC2_SHADOW_FADE_RAMP0`
- OFF: `MC2_LIGHT_MOVER_VIS_GATE`, `MC2_LIGHT_CONE_VIS_GATE`,
  `MC2_LIGHT_BILLBOARD_TWOSIDED`, `MC2_LIGHT_BILLBOARD_FACING`,
  `MC2_LIGHT_REALISM`, `MC2_MOVER_CONTACT_SHADOW`, `MC2_WATER_SHORE_FEATHER`,
  `MC2_TERRAIN_LOD_CHUNK_DIAG`
- `MC2_WATER_SHORE_MEANDER` → **DEFAULT 75.0** (the user-approved value, baked)
- **`MC2_ROAD_PBR` → GHOST.** No reader anywhere in `GameOS/ mclib/ code/
  RenderCore/` — it appears only in `docs/tier1_env_vars.md`,
  `docs/active_campaigns.md` and two status docs. **A documented gate that does
  nothing.** New finding; not previously recorded.
- `MC2_WATER_SHORE_MEANDER_FREQ` → UNRESOLVED (reader found, idiom not one of
  the known ones). Renders as UNRESOLVED with a file:line, never a guess.

Design rule kept: **anything not matching a known idiom renders UNRESOLVED, and
two readers that disagree render UNRESOLVED** — the generator will not guess a
default. `MC2_LIGHT_BILLBOARD_TWOSIDED` is a live example of why this matters:
`.claude/REVIEW-ALLPOSES-1-STATUS.md` still says "default-**ON** (`tgl.cpp:95`)",
but source says **OFF** at `tgl.cpp:106` (`a441b2fa` retired it). The page now
reports OFF.

### `release_assets_src/0.5.2/review_pack.json` — section 14
New section **"14. Lanes & efforts — the full session log"**: **52 lanes**, each
with what it was sent to do, what it *actually* found (premises overturned in
bold), commits, gates with their **render-time default**, colour-coded verdict,
and evidence links. Plus two synthesis blocks: *Findings that outlived their
lane* and *Recurring process failures this session*.

Verdict vocabulary is explicit so a later lane cannot misread it:
`APPROVED BY USER` / `REJECTED BY USER` / `DISPROVEN` / `BLOCKED` / `CANCELLED` /
`IN FLIGHT` / `PARTIAL` / `FIXED` (= agent-verified only, click-through pending).

Marked **REJECTED BY USER**: `MC2_CEMENT_TONE_MATCH`, `MC2_RUNWAY_TONE_MATCH`,
runway-blend rev-1, `MC2_LIGHT_BILLBOARD_FACING` default-ON, shore-feather.
Marked **APPROVED BY USER**: shoreline meander 75, runway rev-2 dilation,
light-realism variant B, light-pool cap 256.

The four findings the brief flagged were each **confirmed from source**, with the
lane that established them, and are called out as standing facts: overlay-channel
roads/runways disjoint from the cement flag; the inverted class-diag legend
(ground truth RED = solid, GREEN = transition, BLACK = colormap); the back-face
guard using the unclamped sun; foliage alpha-test shadows shipped 2026-06-16.

### Extensibility for the live siblings
`ROAD-OVERLAY-LIGHT-1`, `SHADOW-SUNCLAMP-1`, `PROP-LIGHT-CLASS-4`,
`LIGHT-CLUSTER-PERF-1`, `EDGE-LIGHT-EMISSION-1` are present as `IN FLIGHT` rows.
A sibling lands its result by appending/editing **one object** in the manifest's
`lanes` list and re-running the generator. No page rewrite.

## 4. Verification performed

| Check | Result |
|---|---|
| `--check` images | 130 on disk / 130 referenced / **0 orphans / 0 missing** |
| Tag balance (`section table tr td th figure div p ul li details a b code`) | balanced |
| Every local `href`/`src` resolves on disk | **0 broken** (24 new evidence links included) |
| TOC anchors vs section ids | 0 dangling |
| Render idempotence | two consecutive renders byte-identical |
| Live `INDEX.html` == generator output | yes (regenerated as the last step) |
| Rejected gates not recommended | only hits are AUDIT-TRUTH-1's "is REJECTED, **not** recommended" banner |
| Lane rows rendered | 52 |
| Python syntax | OK |

Render-time source truth demonstrated live: mid-lane, the cited
`MC2_LIGHT_REALISM` reader moved `gos_terrain_lighting.cpp:1166` → `:1216`
because `LIGHT-CLUSTER-PERF-1` was editing that file. The **verdict (OFF) did not
change**, and the citation followed the source automatically.

## 5. Adversarial self-review

- *"Would the user still see this on a default launch?"* — For every gate the
  page now answers from source. The honest headline: **most of this session's
  work is default-OFF and therefore not delivered.** The page says so above the
  table, and section 14 repeats it per-lane.
- *Did I re-verify the exe attribution myself?* **No.** `0.4c` was re-deployed
  after ALLPOSES captured, so `c3804cfebb6b` is **ALLPOSES-attested, not
  independently re-confirmed by this lane.** Stated as such above rather than
  claimed as first-hand.
- *Are the 52 rows first-hand?* They are extracted from the lane status docs by
  three subagents and spot-checked against `git log`. They are **faithful to
  those docs**, which are themselves the lanes' own claims — the page presents
  them as such, not as independently re-verified engineering results.
- *Did I weaken anything?* No harness cursor guard touched, no gate default
  changed, no engine source edited, no push, no `--amend`.

## 6. Open / not done

- **Re-capture once the tree settles.** The exhaustive set is one exe old and
  predates `SHADOW-SUNCLAMP-1` (`ad43dab5`), `CEMENT-ATLAS-CRASH-1`
  (`06ffbcf4`) and `ROAD-OVERLAY-LIGHT-1` (`27c70d26`).
- **`MC2_ROAD_PBR` is documented but has no reader** — either delete the docs
  entries or implement it. Not this lane's file.
- `MC2_WATER_SHORE_MEANDER_FREQ` idiom unclassified; add a rule or register it.
- Registering the remaining unregistered gates in `RendererFeatureRegistry.h`
  would let the registry answer directly. Deliberately not done here — that
  header is engine source and outside this lane's claim.
