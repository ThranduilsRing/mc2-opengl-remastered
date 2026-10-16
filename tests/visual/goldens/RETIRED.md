<!-- doc-status: current | reviewed:2026-10-11 -->
# Retired golden sets

A golden set removed from this tree gets an entry here. An absent set with no
record is indistinguishable from a set nobody ever built — the exact failure
mode `GOLDEN-COVERAGE-LEDGER-1` exists to prevent, one level up.

Enforced by `scripts/check-golden-coverage.py`: every id below is in
`RETIRED_SETS`, and the checker FAILs if a retired id reappears on disk.
Restoring one is a deliberate edit to that table, not a silent `git checkout`.

## The three lifecycle states

`scripts/check-golden-coverage.py` carries one table per state, and a set may be
in at most one of them (the checker FAILs on overlap):

| Table | On disk? | Findings |
|---|---|---|
| `LEGACY_WAIVERS` | MUST be present (absent ⇒ stale waiver ⇒ FAIL) | WARN |
| `RETIRED_SETS` | MUST be absent (present ⇒ FAIL, see above) | n/a |
| `PENDING_SETS` | EITHER — deliberately untracked, awaiting a user decision | WARN |

`PENDING_SETS` exists because a candidate set queued for user promotion is
untracked on purpose: committing it pre-empts the decision, deleting it discards
the candidate. So it is present in the deciding user's tree and absent in every
other checkout, and **both are correct**. Excusing such a set via
`LEGACY_WAIVERS` instead made it a hard contract FAIL everywhere but that one
tree — the state simply was not representable. Now it is.

Currently pending: **`v0.5.2-baseline-PROPOSED`** — do not commit, delete, or
promote it; that is the user's call at the promotion gate.

---

## `baseline-6c47e149` — retired by GOLDEN-6C47-AUDIT-1 (2026-07-29)

**Superseded by:** `baseline-5ef261f4` (27 blessed + 12 recorded coverage holes).

**Restore with:** `git checkout 43846054 -- tests/visual/goldens/baseline-6c47e149`
(38 tracked files, intact at `43846054`, the commit immediately before this
retirement; also remove the `RETIRED_SETS` entry in
`scripts/check-golden-coverage.py`, or the checker will FAIL by design).

### Correction to the standing description first

The `LEGACY_WAIVERS` text this retirement replaces said 6c47e149 had "12 regions
unaccounted". That is how the **checker** saw it, but it is not what the set
actually contains. `golden.json` records all 12 under a key named
`refused_regions`, each with `severity`, `check`, `refusal_reason` and
`measured_noise_floor`. The checker reads the key `refused` and requires
`rule` / `reason` / `subject_protected`, so a pre-ledger schema read as silence.

So this is **not** a case of lost documentation, and "back-fill the ledger" was
never the live option. It is retired for what it *is*, below.

### Why retire rather than re-bless

**1. Zero unique coverage.** All 27 blessed regions are already accounted for in
`baseline-5ef261f4` — 24 blessed outright, and the other 3 recorded there as
explicit coverage holes with rule, reason and protected subject:

| region blessed only in 6c47e149 | status in 5ef261f4 |
|---|---|
| `mc2_10_saved_204907/distant_ridge_base` | hole — `DEGENERATE-CROP-REFUSAL-1` |
| `mc2_10_saved_204907/near_base_lighting` | hole — `THREE-RUN-ALL-PAIRS-1` |
| `mc2_10_saved_212258/runway_grid_complete` | hole — `THREE-RUN-ALL-PAIRS-1` |

Deleting the set loses no coverage that is not already written down.

**2. It is strictly dominated.** 5ef261f4 *blesses* three regions 6c47e149 had
refused: `mc2_10_saved_204523/compound_light_wedges`,
`mc2_10_saved_205952/water_shore_grazing`,
`mc2_10_saved_212258/cement_tile_seams`. Same 27-region headline count, more
real coverage.

**3. Two of its three non-overlapping blessings are bad references.**

- `distant_ridge_base` — 300x100, **167 bytes, exactly one unique colour
  (0,0,0)**, registered at severity `critical`. Byte-identical across all three
  runs while sibling regions in the same frames carried 6.5k–36.5k unique
  colours. The rect (px 1300,200 300x100 @ ref 3840x2160) sits in void night sky
  *above* the ridge it claims to frame. It could never fail a diff.
- `runway_grid_complete` — blessed only because 6c47e149's stated rule was
  `"r1-vs-r2 and r1-vs-r3, worst governs"` (`bless_policy.pairs` in its own
  `golden.json`). It never compared **r2-v-r3** — the pair on which 5ef261f4
  later measured it moving **5.16%**. Same structural blind spot flagged
  `pad_outline_fence` at 1.275% and `cement_pads_dark` at 0.825%.

**4. Re-blessing was not available.** The 3 regions are refused under
`THREE-RUN-ALL-PAIRS-1` for run-to-run instability exceeding their own verify
thresholds (floors up to 25.18%). Importing 6c47e149's crops would re-add
references captured on a *different exe* (`6c47e149353c` vs `5ef261f4dd03`)
under the superseded 2-pair rule, making every future verify a coin flip. That
is worse than an honest hole.

**5. Nothing else in it is unique.** Its 10 vendored poses were copied into
`baseline-5ef261f4/poses/` by the successor lane; verified here **byte-identical
for all 10** (`cmp` on each). No tracked file outside `.claude/` status docs and
the two `golden.json` provenance strings referenced the set; the only other hits
are untracked `scratch/` capture artifacts.

## 2026-10-05 reset: `baseline-5ef261f4`, `v0.5.1-baseline`, `v0.5.2-baseline-PROPOSED`

All three were blessed against installs that were archived to
`E:/mc2-archive-2026-10/installs` in the scope-creep reset, so their binaries
are no longer attributable. `v0.5.2-baseline-PROPOSED` was never promoted (it
blessed the 11 regions later refused as unstable plus the degenerate
`distant_ridge_base` crop). Copies of all three: `E:/mc2-archive-2026-10/goldens`.

Replacement: `reset-50331d5` -- 3 deterministic runs of the same 13 poses on
exe `50331d530e1c` (dev lane), 24 regions blessed, 11 refused as unstable
(night lights / lamp cones / runway lights on mc2_10 flicker run-to-run) and the
degenerate crop refused.

## 2026-10-16 CURRENT_GOLDEN switch: `golden-e9e3f19d6-1080p`, `reset-50331d5`

Both are GL-drawn sets. `render_resolution.CURRENT_GOLDEN` now names the Vulkan set `golden-d9e1f3801716-vk-1080p` (36 of 36 regions blessed,
three runs, see `tools/visual_verify/vk_golden_plan.md` section 4), and `verify.py` refuses a candidate drawn by another backend.

- `golden-e9e3f19d6-1080p`: GL at 1920x1080, superseded by the VK set (GL is deleted after VK parity).
- `reset-50331d5`: GL at 3840x2160; already ungradeable at the pinned size.

The directories left the tree in the GL-golden archive commit (tarball + sha256 under
`E:/mc2-archive-2026-10/docs/gl-goldens-2026-10-16/`, `RETIRED_SETS` entries in `scripts/check-golden-coverage.py`).
Restore: extract the tarball into `tests/visual/goldens/` and remove the two `RETIRED_SETS` entries.

## 2026-10-16: `golden-e350d0b4a5c6-vk-1080p` (interim Vulkan set)

**Superseded by:** `golden-d9e1f3801716-vk-1080p` (CURRENT_GOLDEN). The interim set was blessed before the row 1 (prop night pools) and
row 9 (map-edge feather) ports moved 14 of its regions toward GL (`tools/visual_verify/vk_golden_plan.md` sections 3 and 4); it was never
CURRENT_GOLDEN and nothing grades against it. 37 tracked files.

**Archive:** `E:/mc2-archive-2026-10/docs/vk-interim-golden-2026-10-16/golden-e350d0b4a5c6-vk-1080p.tar.gz`, sha256
`04d3298eef9d2bef3d76b7e54381b7f2db4f679e1ee08392cd5bc5044bbe22d1` (extraction diffed against the tree before removal: identical).
Restore: extract it into `tests/visual/goldens/` and remove its `RETIRED_SETS` entry.
