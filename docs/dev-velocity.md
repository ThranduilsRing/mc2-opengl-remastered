# Dev velocity: standing up agent lanes fast (DEV-VELOCITY-LANES-1)

One-pager: how to spawn a new engine lane (worktree + 3rdparty + warm build)
in ~25-35 s instead of ~2.2 min, and how lanes avoid deploy/smoke contention.

## Measured phase costs (16-core machine, NVMe, 2026-07-01)

| Phase | Fresh path | Clone path |
|---|---|---|
| `git worktree add` | 5.9 s (full checkout) | 0.1 s (`--no-checkout`) |
| 3rdparty | 0.8 s (unzip 23 MB zip; most of 3rdparty is git-tracked and comes with checkout) | included in robocopy |
| robocopy donor (1.3 GB, mtimes preserved) | — | 3-4 s |
| build64 path rewrite (572 files) | — | 1-8 s |
| `git reset` + `checkout -- .` | — | 6-7 s |
| cmake configure | ~10 s | not needed (cache cloned) |
| build mc2 | **113 s cold** (596 TUs) | **14 s** sanity/no-op |
| **TOTAL** | **~130 s** | **~25-35 s** |

Reference incremental costs (any warm tree): no-op build 9-15 s; touch one
.cpp -> compile + relink 14-25 s.

## The recipe (use the tool, not the steps)

```sh
cd <any warm worktree>            # this becomes the donor
py -3 tools/spawn_lane.py <lane-name> [--base <sha>] [--suggest-deploy]
# -> A:/Games/mc2-<lane-name> on branch claude/<lane-name>-1, warm build64,
#    sanity-built mc2.exe, ready to edit/build/deploy in ~30 s.
```

`--mode fresh` forces the old path (checkout + unzip + configure + cold
build, ~2.2 min) — still the fallback whenever the clone smells wrong.
`--dry-run` prints the full plan and creates nothing.
`scripts/new_lane.py` remains the tool for lane NOTES/plan scaffolds under
`.claude/worktrees/`; `spawn_lane.py` owns build standup for `A:/Games/mc2-*`
engine lanes.

## Why the clone works (and its limits)

- **mtime preservation is load-bearing.** A fresh checkout writes new source
  mtimes, so copied `.obj` files all look stale -> full rebuild. robocopy
  `/COPY:DAT` keeps donor mtimes; only files that `git checkout` actually
  rewrites (divergence from the base ref) recompile. Proven: after clone the
  build is a true no-op (0 TUs); touching one .cpp recompiles exactly 1 TU.
- **VS build trees are absolute-path poisoned.** vcxproj/sln/CMakeCache/
  CMakeFiles/.tlog all embed the donor path (tlogs in UPPERCASE UTF-16-LE,
  CMakeCache header with a lowercase drive letter). spawn_lane rewrites every
  spelling; a naive build64 copy without the rewrite builds the DONOR's
  sources — silently wrong.
- **Clone from a quiescent donor.** Donor mid-build => torn snapshot. Also,
  `/Zi` objs embed absolute compiler-PDB paths; if the donor's build64 is
  later rebuilt/deleted, a cloned lane's next relink can fail with PDB
  errors — recover with `--clean-first` (~2 min, still cheap).
- **No junctions/symlinks.** CLAUDE.md bans them for build dirs; spawn_lane
  only copies.
- sccache-for-MSVC was evaluated and **rejected**: it refuses `/Zi` (repo
  standard for RelWithDebInfo) and cross-worktree absolute include paths
  defeat its cache key; ROI is poor against a 113 s cold build.

## Deploy + smoke: lanes must not contend

One deploy dir per concurrent lane, always (`.claude/DEPLOY-DIR-LANE-MAP.md`).

1. **Pick a free dir:** `py -3 scripts/check-deploy-target.py --suggest-free
   --branch <lane-branch>` (spawn_lane `--suggest-deploy` prints this for
   you at standup time).
2. **Deploy there:** `py -3 scripts/deploy_payload.py <dir> --source-root .
   --build-dir build64 --exe-name mc2.exe`.
3. **Smoke with an explicit exe:** `run_smoke.py ... --exe <dir>/mc2.exe` —
   this LEASES that folder in `A:/Games/mc2-opengl/.smoke_leases.json`
   (TTL 1 h) so concurrent smokes skip it. Plain `run_smoke.py` with no
   `--exe/--deploy` already auto-acquires the least-recently-used free
   folder — that IS the "--lane auto" behavior; nothing new needed.
4. Never `--no-lease` on shared dirs; never share a deploy dir between two
   live lanes (exe overwrite -> false `crash_silent`).

## Tests

`py -3 -m pytest tools/test_spawn_lane.py -q` — hermetic (temp git repos):
dry-run creates nothing, clone/fresh mode selection, path-variant and
UTF-16/UTF-8 rewrite coverage, binary-file skip, clobber refusals.
