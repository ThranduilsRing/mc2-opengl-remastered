# Deploy isolation — is this install really mine?

`LINK-GUARD-1`, 2026-07-30.

## The incident

Four deploy directories under `A:/Games/mc2-opengl` were one install wearing
four names:

| Directory | In `DEPLOY_ALLOWLIST`? |
|---|---|
| `mc2-win64-mc203-regress` | no |
| `mc2-win64-mc203air` | no |
| `mc2-win64-treesoff-1` | no |
| `mc2-win64-v0.4d-rc1` | **yes** |
| `releases/mc2-win64-refbuild-cf97ed52` | no (partial, 751 files) |

Measured, not assumed: **1530 of 1530** written artifacts in
`mc2-win64-v0.4d-rc1` were hardlinked to at least one of the others —
`mc2.exe`, the entire 204-file shader tree, every DLL, `Mission Editor.exe`,
`Mission Editor.pdb` (102 MB), `art.fst`, `.deployed_manifest.csv`, and
`.mc2_deploy_lane.json` — the lane lock itself.

`scripts/deploy_payload.py` copies with `shutil.copyfile`, which opens the
destination `"wb"`. That **truncates and rewrites the existing inode**; it does
not unlink first, so it does not break the link. Reproduced directly:

```
A/mc2.exe and B/mc2.exe hardlinked; A reads b'ORIGINAL-BASELINE-BYTES'
shutil.copyfile(new, B)
-> A now reads b'NEW-LANE-BYTES'   (inode unchanged, nlink still 2)
```

Deploying into any one of those directories silently rewrote the others. It
cost real evidence twice in one day: one lane destroyed `mc2-win64-v0.4` as a
comparison baseline mid-session, and another lane's 07-28/29 findings turned
out to rest on self-report because every lane had been writing bytes somebody
else was reading.

## Why nothing caught it

Every existing guard answered a **different** question, and answered it
correctly:

| Guard | Question | Answer |
|---|---|---|
| `DEPLOY_ALLOWLIST` | is this a canonical folder? | yes — `v0.4d-rc1` is on the list |
| `.mc2_deploy_lane.json` | does another lane hold THIS PATH? | no |
| `.smoke_leases.json` | is another lane smoking THIS PATH? | no |
| `deploy_status()` | is the exe at THIS PATH stale? | no |
| `register_lane()` `deploy_lane_clash` | did another lane name THIS STRING? | no |

All of them are **path-keyed**. A hardlink is precisely the case where two
paths are one object, so a path-keyed registry reports a directory as free
while writing it mutates somebody else's install. There was no question anyone
could ask that would have returned the true answer.

Note the sharpest edge: `v0.4d-rc1` is **on the allowlist**, so the guard meant
to keep lanes tidy was actively steering them into the shared group.

## Hardlinks are not the enemy

A: had ~9–11 GB free while a single install measures **12.26 GB**. Per-lane
full copies are not affordable — and an earlier lane got a private 7 GB install
for free precisely *by* hardlinking, which was the right instinct. The rule is
not "no hardlinks":

> Data that is only ever **read** — share it; linking is free and correct.
> Artifacts that get **written** — must be a private inode per lane.

Measured on the real `mc2-win64-v0.4` install:

```
link (read-only data)   : 39034 files, 11.62 GB   [costs nothing]
copy (written artifacts):  1101 files,  0.64 GB   [the real cost]
saved by linking        : 94.8%
```

## Make a private install

```sh
py -3 scripts/make-isolated-deploy.py \
    --from A:/Games/mc2-opengl/mc2-win64-v0.4 \
    --to   A:/Games/mc2-opengl/mc2-win64-<lane>

# see the cost first, write nothing
py -3 scripts/make-isolated-deploy.py --from ... --to ... --dry-run
```

It hardlinks the data class, byte-copies the mutated class, and then **runs the
guard against what it just built and requires verdict OK** — it will not leave
a booby-trapped install behind and print a hopeful summary.

The result is deliberately not in `DEPLOY_ALLOWLIST` (that list is about
install *count*, a separate concern), so deploy into it with
`--allow-new-target`.

It refuses, before writing anything, if `--from` and `--to` are on different
volumes: `os.link()` cannot span volumes, so every file would fall back to a
byte copy and the "cheap" 0.64 GB operation would land as 12 GB — past a disk
guard that already approved the smaller figure.

## Check a dir yourself

```sh
py -3 scripts/check-hardlink-isolation.py --target A:/Games/mc2-opengl/<dir>
```

Verdicts and exit codes (the `check-disk-space.py` convention):

| Verdict | Exit | Meaning |
|---|---|---|
| `OK` | 0 | no written artifact shares an inode. The install is yours. |
| `LINKED-DATA` | 1 | written artifacts private, read-only data shared. **This is the good state** (`--data-links` to see it). |
| `ERROR` | 2 | could not check. Never means "fine". |
| `SHARED` | 3 | writing this directory mutates another install. |

### What counts as "written"

`MUTATED_ROOTS` in the guard — the deploy-written plus runtime-written surface:

- `<root>/*` (non-recursive) — exe, pdb, dlls, `*.bat`, `options.cfg`,
  `.deployed_manifest.csv`, `.mc2_deploy_lane.json`
- `shaders/**` — every deploy rewrites the whole tree
- `tools/**` — terrain_gen authoring tree (support payload)
- `data/defs/**` — UI defs payload

It is a list of **roots**, not extensions, on purpose: an extension allowlist
drifts silently the moment somebody adds a payload class, while a root list
over-approximates, so new payload files are protected by default.
Over-approximating fails **loud** (a spurious refusal someone investigates);
under-approximating fails **open** — the exact silence this incident was made
of.

`deploy_payload.py` does not rely on the heuristic at all: it passes the
literal relpaths it is about to write via `--payload-list`, so at the site that
matters the classification is exact.

### Why `st_nlink` is the primary signal

Detection does **not** depend on enumerating sibling directories. `st_nlink > 1`
is the filesystem's own answer to "does another name point at these bytes", and
it holds even when the other name lives somewhere the script would never think
to look. Naming *which* directories is best-effort attribution layered on top;
it can come back empty and the refusal still stands. A guard whose correctness
depended on a hardcoded search list would have the same blind spot as the
path-keyed registries it exists to backstop.

## Where it is enforced

| Site | Behaviour |
|---|---|
| `scripts/deploy_payload.py` | Preflight after payload enumeration, before any byte is written. `SHARED`/`ERROR` — and a guard that is **missing or un-runnable** — are all **fatal**. |
| `deploy_status(lane)` | New `link_group` field; new verdict `SHARED-INSTALL`, ranked second (below `BROKEN-TREE`, above `CONTENDED`). |
| `register_lane(...)` | New `link_group` and `same_bytes_as` fields. `same_bytes_as` names lanes holding a **different path that is the same bytes** — the case `deploy_lane_clash` structurally cannot see. |
| `scripts/check-contracts.sh` | `link_guard_selftest`, `iso_deploy_selftest`. |

Failing open was rejected for the same reason it was rejected for the disk
guard in `c5d934d5`: this guard's entire purpose is to prevent a silent,
hard-to-attribute corruption, and degrading to unguarded on a missing
dependency retires the protection exactly when the checkout is in a state
nobody has verified — and it does so via a log line, which is what we already
had.

Deliberate, loud escape hatch: **`MC2_DEPLOY_ALLOW_SHARED_LINKS=1`**. Bypassing
is then a decision someone made on purpose and can be found in the environment
afterwards.

## Not done here

The five directories above are **still shared**. Several were mid-investigation
when this landed, and rewriting a live lane's install to fix a guard is the
same class of mistake the guard exists to prevent. Detection is in place and
loud; migration is a separate, deliberate decision.
