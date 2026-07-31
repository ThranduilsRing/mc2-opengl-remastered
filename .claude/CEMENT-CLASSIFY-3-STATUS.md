# CEMENT-CLASSIFY-3 — STATUS

**Lane:** CEMENT-CLASSIFY-3
**Worktree/branch:** `A:/Games/mc2-main` @ `claude/nifty-mendeleev`
**HEAD at session start:** `ffc0f10a`
**Commits landed by this lane:** NONE (see "Why nothing was built")
**Date:** 2026-07-26

---

## Verdict in one line

The lane's briefed root cause is **correct in substance but was already found,
already instrumented, and already partly mis-fixed by two sibling lanes**; the
one structural fact nobody had written down is that **every existing cement
lever is incapable of touching the defect class the user circled**. No capture
evidence was produced, because a live sibling lane holds the tree dirty with
uncommitted edits to the capture harness itself.

---

## META REVIEW — brief premises checked against the reader source

| Brief premise | Verdict | Evidence |
|---|---|---|
| "tree CLEAN as of `1779bf1d`" | **FALSE, twice over** | HEAD is `ffc0f10a`, not `1779bf1d`. Tree has 2 tracked modifications (`scripts/capture_baseline.py`, `scripts/sweep_campaign.py`) + 192 untracked. `preflight` returns `safe_to_touch=false`. |
| "`MC2_CEMENT_ALPHA_ATLAS` … is the lever intended to close the drop … its consumer side was never finished" | **STALE** | The consumer side **shipped** in `c3804cfe` (CEMENT-ALPHA-LAND-1, 2026-07-25 12:57) — atlas admit, `bit23 OWN_ALPHA` word, frag composite branch, class-diag colour 6 (cyan), `alpha_rescued` counter. `1779bf1d` is a *later* WIP refinement stacked on top of it, not the unfinished consumer. |
| "class diag: green = transition-classified, red = not … tiles that are plainly pavement render RED ⇒ the classifier never identifies them" | **LEGEND INVERTED** | Shader ground truth `shaders/terrain_lod_chunk.frag:1202-1215`: **RED = SOLID cement-atlas replacement; GREEN = TRANSITION; BLACK/dim (`base*0.35`) = baked colormap only.** A pavement tile rendering RED is *correctly* cement-classified. The defect marker is **BLACK**, not red. `RUNWAY-APRON-TONE-2-STATUS.md:52-53` states the legend correctly; the brief and the code comment at `gos_terrain_indirect.cpp:1373` both repeat the inverted version. |
| "the 87% figure may be an UNDERCOUNT if the denominator only counts tiles already recognised as transition — CHECK" | **ALREADY CHECKED, and correct** | Exactly this was found and instrumented by CEMENT-UNIFORM-2 in `a25037cf`, gated to `MC2_TERRAIN_LOD_CHUNK_DIAG` bit 524288 by `31ff3e8d`. See `[CEMENT_PAVEMENT_GAP v1]`, `gos_terrain_indirect.cpp:1217-1245,1339-1382`. The numbers have **never been run**. |
| "the magenta mechanism's 'already fixed in v0.5.0' citation is FACTUALLY WRONG — re-open" | **HALF RIGHT — and the correction matters** | `git merge-base --is-ancestor 1d78f204 v0.5.0` → **NO** (audit correct). But `… 1d78f204 HEAD` → **YES**. `1d78f204 fix(terrain): honor magenta colorkey on keyed transition tiles (roads)` **is live on the current branch**. The fix exists; it merely missed the v0.5.0 cut. If the user saw pink on a v0.5.x build, that is expected and needs a release, not a re-open. |
| "siblings live — check_conflict" | **SEVERE** | 8 of my claimed paths are claimed by **two** sibling lanes with my exact mission (below). |

### Sibling collision (the headline process finding)

`register_lane` + `list_lanes` show this lane is the **third** concurrent attempt
at the same work, all on the same worktree and the same `0.4c` deploy lane:

| Lane | Registered | Mission (its own note) |
|---|---|---|
| `CEMENT-ALPHA-LAND-1` | 2026-07-25 17:51Z | "verify MC2_CEMENT_LIT ON arm + land alpha-cement atlas fix (closes 87%…)" |
| `CEMENT-UNIFORM-2` | 2026-07-25 18:11Z | "mc2_03 intro concrete/road uniformity; verify MC2_CEMENT_LIT ON arm; classification gap; magenta" |
| `CEMENT-CLASSIFY-3` (this) | 2026-07-26 00:23Z | same three items again |

All three were briefed to "verify the `MC2_CEMENT_LIT` ON arm". It is **still
not verified** — now across three lanes and two calendar days, each blocked by
the same shared-worktree dirtiness. That is a process defect, not a rendering
defect, and it is the reason this file exists.

---

## The one genuinely new technical finding

**Every existing cement lever is structurally incapable of reaching the
pavement the user is complaining about.**

Two independent per-tile channels:

- **Base terrain slot** — `getTexture(my,mx) & 0xFFFF`. `isCement(slot)` is a
  flag test on that slot (`mclib/terrtxm.h:362`, `MC2_TERRAIN_CEMENT_FLAG`);
  the cement base-types are `BASE_CEMENT_TYPE 10` and `START..END_CEMENT_TYPE
  13..20` (`mclib/terrtxm.h`).
- **Overlay id** — `getOverlay(my,mx,…)`, a *disjoint* `Overlays` enum
  (`mclib/mapdata.h:43-57`): `PAVED_ROAD=1`, `RUNWAY=7`,
  `X_PAVED_ROAD_BRIDGE=15`, …

Roads and runways are **overlays painted over an arbitrary base type**. A runway
laid across dirt has `isCement(slot) == false`.

Now trace `PopulateRecipeCementWords` (`gos_terrain_indirect.cpp:1249-1338`):

```
if (idx != 0xFFFF)                                   -> SOLID cement   (RED)
else if (isCement(slot) && isAlpha(slot)) {          -> TRANSITION     (GREEN)
      … maskId/corner path                                            (baked)
      … else if CementAlphaAtlasEnabled() … OWN_ALPHA rescue          (CYAN)
      … else ++transitionDroppedNoLayer / NoMask
}
// no else — cementWord stays 0                      -> COLORMAP ONLY  (BLACK)
```

A paved overlay on a non-cement base **falls off the bottom of that chain**. It
increments no drop counter, so `[CEMENT_TRANSITION_DROP v1]`'s 278/320 and
266/778 are blind to it — the brief's suspicion is right, and the true
denominator is what `[CEMENT_PAVEMENT_GAP v1]` was built to report.

Critically, **both** repair levers are nested inside the
`isCement && isAlpha` guard and therefore cannot fire on this class:

- alpha-atlas rescue — nested at `:1309`, inside the `:1264` guard; and
  `g_cementAlphaLayerIndexBySlot` is only ever populated for
  `isCement && isAlpha` slots (`:1789-1790`).
- `MC2_CEMENT_TRANSITION_FALLBACK` — explicit early-out
  `if (!(tt->isCement(slot) && tt->isAlpha(slot))) continue;` at `:1401`.

**Consequence:** `c3804cfe` genuinely closes the *transition-tile* drop, but it
cannot close the user's "missing part of the runway" where the runway crosses
open ground — the two are different defect classes that the prior lanes'
documentation conflates. Turning `MC2_CEMENT_ALPHA_ATLAS` on will improve some
tiles and leave the circled ones untouched. Any future lane that reports the
gate as "the fix" without separating these classes is repeating the error.

This also re-frames `MC2_CEMENT_LIT`: since the overlay-pavement class is
rendered as *fully lit colormap* and the apron as *lighting-exempt slab*,
`MC2_CEMENT_LIT` is the only landed lever that acts on **both** sides of that
boundary. It remains the right thing to verify first — the brief is correct
there.

---

## Why nothing was built, committed, or captured

`scripts/capture_baseline.py` (+45/−8) and `scripts/sweep_campaign.py` (+27/−6)
are **uncommitted, and belong to a live sibling lane**: `VERIFY-INTEGRITY-3`,
registered 2026-07-26 00:22Z — *one minute* before this lane — with the note
"fix verification-lying infra (bookmarks, ungated printf, REVIEW pack …)".
File mtimes 2026-07-25 19:24 local confirm active editing.

Two independent blocks follow, and neither is mine to clear:

1. **`deploy --require-clean` is impossible.** The tree is dirty with another
   lane's work. Committing or stashing it would scoop a live lane — the exact
   failure `RUNWAY-APRON-TONE-2-STATUS.md:151-160` records as the reason
   `MC2_CEMENT_LIT` went unverified the first time. Doing it again would make
   this the second scoop and the third unverified lane.
2. **The evidence would not be trustworthy if I could.** The captures would run
   through the very harness a concurrent lane is mid-repair on, for the stated
   reason that it *lies*. Producing verdict shots from it and calling
   `MC2_CEMENT_LIT` verified would be precisely the failure mode
   `VERIFY-INTEGRITY-3` exists to stop.

The deployed `0.4c` exe was also checked and is stale — it contains **zero**
occurrences of `MC2_CEMENT_ALPHA_ATLAS` or `MC2_CEMENT_LIT`, i.e. it predates
all of this work. Nothing could have been verified against it either.

**Nothing in this document is capture-derived.** Every claim above is from
source, git, or the gate registry, and each is cited. The user-facing defects
remain **unfixed and unverified**.

---

## Gate state (checked, not assumed)

| Gate | Default | `gate_status` | Note |
|---|---|---|---|
| `MC2_CEMENT_LIT` | 0.0 (OFF) | **OK** — registered, tier1-documented, smoke-allowlisted | ON arm still never captured (3 lanes) |
| `MC2_CEMENT_ALPHA_ATLAS` | OFF | **GAPS** — registered + smoke-allowlisted, **UNDOCUMENTED** in `docs/tier1_env_vars.md` | cannot reach the overlay-pavement class |
| `MC2_TERRAIN_LOD_CHUNK_DIAG` bit 524288 | unset | drives `[CEMENT_PAVEMENT_GAP v1]` | **never run — the decisive missing number** |
| `MC2_CEMENT_TONE_MATCH` / `MC2_RUNWAY_TONE_MATCH` | OFF | USER-REJECTED | do not revive |

---

## Recommended next lane (do these in order, on a QUIET worktree)

1. **Serialise the cement lanes.** Retire `CEMENT-ALPHA-LAND-1` and
   `CEMENT-UNIFORM-2`, land or drop `1779bf1d`, and run **one** lane. Three
   concurrent lanes on one worktree and one deploy slot is what has blocked
   verification twice.
2. **Run the number that already exists.** `MC2_TERRAIN_LOD_CHUNK_DIAG=524288`
   across mc2_01/03/05/10/24 and read `[CEMENT_PAVEMENT_GAP v1]`
   `paved_no_cement / paved_overlay`. This needs **no code change** and sizes
   the real defect for the first time.
3. **Then** verify the `MC2_CEMENT_LIT` ON arm at 204937 + a night pose, and
   prove mc2_05's user-approved night lighting unchanged.
4. Only then design the overlay-pavement fix — it needs a **new** predicate
   keyed on `getOverlay`/`IsPavedOverlay`, outside the `isCement && isAlpha`
   guard. Every existing lever is provably inside it.
5. Fix the inverted class-diag legend in the comment at
   `gos_terrain_indirect.cpp:1373` before it misleads a fourth lane.
6. Magenta: not a re-open. `1d78f204` is on the branch; it needs a **release**.

---

## Bookends

- **META REVIEW:** done first; six brief premises checked against source/git,
  five corrected. Recorded above.
- **ADVERSARIAL SELF-REVIEW:** *"Would the user still see this on a default
  launch?"* — **Yes, all six defects, unchanged.** This lane shipped no code and
  claims no fix. The one non-trivial assertion made (that every cement lever is
  unreachable for overlay pavement) was attacked by re-reading the nesting at
  `:1264/:1309/:1401` and the population guard at `:1789-1790`, and holds.
  No gate was added, so no registration/allowlist claim is made; no build was
  produced, so no CLEAN-build or in-binary gate-string evidence is claimed —
  and none is implied anywhere above.
