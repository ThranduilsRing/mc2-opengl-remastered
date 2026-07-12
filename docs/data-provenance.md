# Game Data Provenance

Answers GitHub issue #32 ("Data Files: Patches Applied?"). Distinguishes
what is verifiable from the repository from what requires upstream
confirmation. Nothing here is asserted beyond what the repo proves.

## Source of the data

The game data is **not** a copy of retail CD files and is **not** the
result of running `mc2_patch.exe` against a retail install. It is built
from source assets in the **`alariq/mc2srcdata`** repository
(`https://github.com/alariq/mc2srcdata.git`). It is **not** a git
submodule/dependency (no `.gitmodules` entry) — it is acquired by a
plain `git clone` at the repo root and the resulting `mc2srcdata/`
directory is gitignored (`.gitignore:104`):

```bash
git clone https://github.com/alariq/mc2srcdata.git
```

alariq's own description: *"data files for my Mech Commander 2 linux
port."* It is a curated, build-scriptable asset tree, not a raw game
dump.

## How a release's data is produced

`mc2srcdata/build_scripts/` drives the build via GNU Make
(`make all`). It invokes the data tools built alongside the engine:

- `aseconv`, `makefst`, `makersp`, `pak`, `text_tool`

The `.fst` archives shipped in a release (e.g. `mission.fst`) are
**produced by `makefst` from alariq's source tree** at build time.
They are not lifted from a patched retail install. This is why
`PurBonus*.fit` etc. exist inside `mission.fst` rather than as loose
files (see issue #8 and `memory/fst_forward_slash_invariant.md`).

## This project's modifications on top of alariq

Tracked in `git -C mc2srcdata log`. Notably:

- `19e8be1` art 4x upscales trimmed to UI/portrait-only; tgl upscales
  archived
- README / data-build fixes (`bf8a08e` makersp `*` substitution fix,
  etc.)

Optional modern assets (4x upscales, `art_4x_gpu/`) are **additive
sidecars**: a stock install must remain playable without them (see
`memory/stock_install_must_remain_playable.md`).

## The `mc2_patch.exe` question (issue #32) — OPEN

Issue #32 observes that "at least a few files from `mc2_patch.exe` are
in the 0.3 release." This is **consistent with** alariq's source data
already being at official-patch content level, but:

- Nothing in alariq's `README.md`, `build_scripts/`, or this project
  states whether the upstream source tree reflects the retail 1.0
  patch.
- There is no patch-application step anywhere in the build; data is
  generated from alariq sources, so "which retail patch level" is an
  **upstream (alariq) property**, not something this project applies or
  can prove from its own tree.

**Status: unresolved at the repo level. Requires upstream/maintainer
confirmation** of alariq's source patch level. Do not assert a
definitive yes/no without that — the absence of a patch step means the
honest answer is "inherited from alariq, level undocumented," not
"patched" or "unpatched."

## One-line summary for users

Data is built from `alariq/mc2srcdata` (a Linux-port asset tree), not
from retail CDs or `mc2_patch.exe`; this project adds optional upscaled
art sidecars. Whether alariq's sources are at retail-patch level is
undocumented upstream and is the only open part of issue #32.
