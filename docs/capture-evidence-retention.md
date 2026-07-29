# Capture & evidence retention policy — PROPOSED

**Status:** PROPOSAL, not yet imposed. Nothing in this document has been applied
to any existing lane. It needs a decision before it becomes policy.
**Lane:** DISK-GUARD-1 · **Date:** 2026-07-29
**Trigger:** the ENOSPC incident of 2026-07-29 (see below).

---

## 1. What happened, and why this is the root cause

The `A:` drive reached 0 bytes free. Because no script in the repo checked free
space, the failure did not present as an error — it presented as **corruption**.
A write hit ENOSPC part-way through and **truncated three files to zero bytes**.
They were recovered only because sibling worktrees happened to hold copies. Four
lanes lost hours of work they could not commit.

`scripts/check-disk-space.py` (DISK-GUARD-1) now makes that failure loud and
early. But the guard treats a symptom. The reason the drive filled is:

> **`.claude/` is being used as a multi-GB binary capture store inside git
> worktrees.**

Measured on 2026-07-29:

| Path | Size |
|---|---|
| `mc2-vk-interop-bridge/.claude` | 23.4 GB |
| └ `.claude/mech-parity` | 13.7 GB |
| └ `.claude/adjudication` (incl. a 1,010,693,435-byte `.rdc`) | 1.68 GB |
| `mc2-deferred-1/.claude/captures` (incl. a 925,638,780-byte `.rdc`) | 11.3 GB |
| **Total across two lanes** | **34.7 GB** |

`.claude/` is a config-and-notes directory. It is now holding PNG, TGA and
RenderDoc payloads, including **two single `.rdc` files near 1 GB each**. This
is not a one-time accident; it is the default outcome of how capture output
paths get chosen today.

## 2. Why it keeps happening — the mechanism

There *is* a natural chokepoint, and it is already well-defined:

| Layer | Location |
|---|---|
| Engine reads the capture dir | `GameOS/gameos/gos_visual_capture.cpp:497` and `:513` — `getenv("MC2_VISUAL_CAPTURE_DIR")` |
| Engine reads the RenderDoc path template | `GameOS/gameos/gos_rdoc_capture.cpp:106` — `getenv("MC2_RDC_CAPTURE_PATH")` |
| Tracked driver sets it | `scripts/run_visual_capture.py` (`DEFAULT_OUT = <repo>/tests/visual/captures`) |
| Tracked driver sets it | `scripts/renderdoc_capture.py` (`--out`, required) |

**The tracked tooling is not the problem.** The problem is that roughly a dozen
*ad-hoc lane driver scripts* — `mech-parity/field_oracle.py`,
`mech-parity/terrain_oracle.py`, `vk-post-fog/fog_oracle.py`,
`vk-water-tier1/water_oracle.py`, and siblings — each set
`MC2_VISUAL_CAPTURE_DIR` themselves, to a path they hardcode, and every one of
those scripts *lives inside `.claude/`* and writes next to itself. Each is
individually reasonable. Collectively they are an unbounded binary store inside
a git worktree, with no owner and no expiry.

Two aggravating factors, both fixed in this lane:

- **Mis-named captures went unnoticed.** A ~1 GB file on disk was named
  `vk_airport_{frame}_frame41.rdc` — a literal, unexpanded `{frame}`.
  `scripts/renderdoc_capture.py` passed `--out` through verbatim to RenderDoc's
  `SetCaptureFilePathTemplate`, so a caller's unexpanded placeholder silently
  became part of a gigabyte filename. `--out` now expands `{mission}`/`{frame}`
  and **hard-errors on any surviving brace**.
- **Nothing measured the cost before paying it.** Both capture drivers now run
  the DISK-GUARD-1 advisory probe before launching the engine.

## 3. The proposal

### 3.1 Captures leave the worktree

Large capture/evidence binaries move to a **capture root outside every
worktree**, on a volume that is not the build surface:

```
E:\mc2-captures\<lane>\<YYYY-MM-DD>\<tag>\
```

selected by a single environment variable, defaulting per-machine:

```
MC2_CAPTURE_ROOT = E:\mc2-captures      # default; override per machine
```

`MC2_VISUAL_CAPTURE_DIR` and `MC2_RDC_CAPTURE_PATH` keep their current meaning —
they stay the engine-side chokepoint and need no engine change. What changes is
that **drivers derive them from `MC2_CAPTURE_ROOT`** instead of hardcoding a
path beside themselves.

Rationale for a variable rather than a hardcoded `E:`: `E:` is this machine's
12.4 TB archive volume, which is a local fact, not a repo fact.

### 3.2 What may stay inside a worktree

| Kind | Where | Why |
|---|---|---|
| Notes, plans, recon, status `.md` | `.claude/` | Small, text, reviewable, the directory's actual purpose |
| A handful of small reference PNGs deliberately cited by a doc | `.claude/` | Bounded, and the evidence is the point |
| Golden/baseline images under test control | `tests/visual/` | Tracked, versioned, intentionally durable |
| **Capture sweeps, `.rdc` files, per-run PNG/TGA/JSON dumps** | **`MC2_CAPTURE_ROOT`, never a worktree** | Unbounded, binary, untracked, single-use |

Proposed hard line: **no untracked binary over 50 MB inside a worktree, and no
`.rdc` inside a worktree at all.**

### 3.3 Retention

| Class | Retain | Then |
|---|---|---|
| `.rdc` RenderDoc captures | 7 days | Delete. They are ~1 GB each and re-capturable from a known sha + mission + frame. |
| Per-run capture sweeps (PNG/TGA/JSON) | 30 days | Move to `E:\mc2-archive\` |
| Sweeps cited as evidence in a shipped decision | indefinite | Move to `E:\mc2-archive\`, and record the archive path in the doc that cites them |
| Smoke run artifacts (`tests/smoke/artifacts/`) | 7 days live | Archive the tail (1,721 run dirs existed on 2026-07-29, 1,541 of them older than 7 days) |

**Deletion rule:** only `.rdc` files are ever deleted on schedule, because only
they are cheaply reproducible *and* individually enormous. Everything else is
**moved, never deleted** — the 2026-07-29 reclaim recovered ~36 GB by moves
alone, and the evidence base for parity claims is not in git.

### 3.4 Enforcement (proposed, not implemented)

1. A `check-capture-hygiene.py` contract that fails when a worktree contains an
   untracked binary over 50 MB or any `.rdc`. Cheap (`git status --porcelain`
   plus a size stat), read-only, registerable in `check-contracts.sh`.
2. A shared capture-path helper (proposed location: a `capture_paths` module
   under `scripts/smoke_lib/`, which does not exist yet) exposing
   `capture_dir(lane, tag)`, so a new lane driver gets the right path by
   default instead of by discipline. **This is the load-bearing item** — the
   current failure is that doing the wrong thing is easier than doing the right
   thing.
3. Extend `.gitignore` to cover `.claude/**/*.rdc` so the mistake at least never
   reaches a commit.

None of these are done. (1) and (3) are small. (2) is the one worth doing
properly, and it should be designed with whoever owns the oracle-driver pattern.

## 4. What this lane actually implemented

Guard and bug fixes only — **no cleanup was imposed and no existing lane's files
were moved**:

- `scripts/check-disk-space.py` — sized free-space preflight, named verdicts,
  distinct exit codes, `[DISK_GUARD]` prefix, `--selftest` ratchet.
- Fatal call sites: `scripts/build_release.sh` (before `mkdir`/`mktemp -d`),
  `scripts/deploy_payload.py` (immediately after `enumerate_payload()`, using
  the measured payload size).
- Advisory call sites: `scripts/run_visual_capture.py`,
  `scripts/renderdoc_capture.py`.
- `scripts/renderdoc_capture.py` — `{mission}`/`{frame}` expansion plus a hard
  error on unexpanded placeholders.
- `GameOS/gameos/gameos.cpp` — `gos_GetDriveFreeSpace()` now honours its `Path`
  argument (it passed `NULL`, reporting the wrong drive) and returns 0 rather
  than uninitialised garbage when the query fails.
- `scripts/check-contracts.sh` — registers `disk_guard_selftest`.

## 5. Open decisions for the user

1. Adopt `MC2_CAPTURE_ROOT` and the `E:\mc2-captures\<lane>\<date>\<tag>\`
   layout? (Section 3.1)
2. Adopt the 50 MB / no-`.rdc`-in-worktree hard line? (Section 3.2)
3. Adopt the retention table, including "`.rdc` is the only class ever deleted
   on schedule"? (Section 3.3)
4. Build the `capture_paths.py` helper and the `check-capture-hygiene.py`
   contract? (Section 3.4)
