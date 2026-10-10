<!-- doc-status: current | reviewed:2026-10-11 -->
# Chapter 0 — Getting Started With MC2 Data Mods

> **AVAILABILITY**
> - **Works on:** this checkout (any branch that has `mclib/file.cpp`'s mod-overlay block, `tools/mc2mod/mc2mod.py`, and `tools/mc2_launcher/`). Verified present on the current branch at the time this doc was written.
> - **Core env var:** `MC2_ACTIVE_MOD` — unset by default. Unset = base game, `g_modIndex` stays empty, zero overlay overhead.
> - **Diagnostics env vars:** `MC2_LOG_FILE_RESOLVE` (0/1/2, default 0 = off), `MC2_RESOLVE_TRACE_FILE` (path, only read when `MC2_LOG_FILE_RESOLVE=2`), `MC2_REBUILD_MOD_CACHE` (any value = force rebuild, default off).
> - **No build required.** Everything in this chapter is data-only and works against a compiled `mc2.exe` you already have — you never touch `shaders/` or C++ source.
> - Two env vars you may see referenced elsewhere — `MC2_EQUIPMENT_ADD` and `MC2_UNIT_PROFILE_DATA` — **do not exist on this checkout.** They live only on the unmerged local branch `claude/unit-profile-seam` (tip `59f16541`). If an example doc in this collection marks a feature "Coming soon", that's why.

---

## 1. Three different things people call "modding MC2"

This collection is about exactly one of these three:

| Kind | What it touches | Needs a C++ rebuild? | Docs |
|---|---|---|---|
| **Data mods** (this collection) | Files under a game's `data/` tree only — CSVs, `.fit` ini-style files, textures, glTF meshes, JSON manifests | No | You're reading it |
| **Engine-source modding** | C++ engine code, build system, shader compile flags | Yes | [`docs/modding-guide.md`](../../modding-guide.md) |
| **Renderer contributor docs** | Frame pipeline internals, render-contract buckets, debugging GPU issues | Yes (you're editing the renderer) | [`docs/modding/rendering-overview.md`](../rendering-overview.md), [`docs/modding/renderer-feature-flags.md`](../renderer-feature-flags.md), [`docs/modding/debugging-render-issues.md`](../debugging-render-issues.md) |

If you just want to swap a weapon stat, add a mech variant, or drop in a replacement building model, you're in the right place and you will never need Visual Studio.

## 2. What a "mod" is here

A data mod is a folder under the game's `mods/` directory (next to `mc2.exe`) that **shadows** files in the base game's `data/` tree. When the mod is active, any file the mod ships at a given relative path is used *instead of* the base game's file at that same path — whole file, no merging, no diffing. Files the mod doesn't ship fall through to the base game unchanged.

This is implemented in `mclib/file.cpp` (`InitModSearchPaths`, `mclib/file.cpp:679`), which builds an in-memory index (`g_modIndex`) at startup and consults it before any base-game file lookup.

### Folder anatomy

```
<game-folder>/
  mc2.exe
  mods/
    my-cool-mod/
      mod.json              <- optional but recommended
      data/
        objects/
          compbas.csv        <- shadows data/objects/compbas.csv
        tgl/
          atlas.ini          <- shadows data/tgl/atlas.ini
      .modindex-cache        <- auto-generated, do not hand-edit or commit
```

Only files under `mods/<id>/data/` are indexed and considered for overlay — `mod.json` itself lives one level up, at `mods/<id>/mod.json`, not inside `data/`.

### `mod.json` schema

```json
{
  "id": "my-cool-mod",
  "name": "My Cool Mod",
  "type": "campaign",
  "description": "One-line blurb shown in the launcher.",
  "dependencies": ["some-shared-asset-pack"]
}
```

| Field | Read by | Required? | Notes |
|---|---|---|---|
| `id` | engine (`file.cpp:536`) + launcher | No — falls back to the folder name if `mod.json` is missing or `id` is blank | This is the value you pass to `MC2_ACTIVE_MOD` |
| `name` | launcher only | No | Display name in the picker |
| `type` | launcher only (`tools/mc2_launcher/mc2_launcher.cpp:96-107`) | No, defaults to `"campaign"` | Only `type: "campaign"` mods are shown as selectable entries in the launcher. `type: "dependency"` mods are hidden from the picker and are mounted automatically when a campaign mod that lists them in `dependencies` is selected. |
| `description` | launcher only | No | Shown under the mod name in the picker |
| `dependencies` | engine (`file.cpp:538`, `693-740`) | No | Array of other mod folder names to mount underneath this mod, in the order given |

If `mod.json` is entirely missing, the engine still works — it treats the folder name as the mod id and loads it with no dependencies (you'll see `[mod] warning: no mod.json for '<name>', loading with no dependencies` on stdout).

## 3. Whole-file shadowing and priority

Priority, highest to lowest:

```
active mod  >  dependency[0]  >  dependency[1]  >  ...  >  base data/  >  FastFiles  >  CD
```

- The **active mod** (the one named by `MC2_ACTIVE_MOD`) always wins.
- Its declared `dependencies` are indexed next, in the order listed in `mod.json` — the first dependency in the array outranks the second, and so on.
- Anything none of those ship falls through to the base game's `data/` tree, then packed FastFiles, then (practically unreachable on a modern install) the original CD path.
- Indexing is **first-wins**: once a relative path (e.g. `data/objects/compbas.csv`) is claimed by a higher-priority layer, a lower-priority layer's file at the same path is simply never looked at. There is no field-level or line-level merging — if you want to change one row of a CSV, you ship the *entire* CSV.
- Matching is case-insensitive and backslash/forward-slash-insensitive (`mclib/file.cpp:215`, `NormalizeKey`).

This whole mechanism is session-scoped: it's built once at process startup from the `MC2_ACTIVE_MOD` value and never changes for the life of that `mc2.exe` process.

## 4. Setting `MC2_ACTIVE_MOD`

### Option A — PowerShell (manual / scripting)

```powershell
cd <path-to-your-game-folder>
$env:MC2_ACTIVE_MOD = "my-cool-mod"
.\mc2.exe
```

To go back to the base game in the same shell:

```powershell
Remove-Item Env:MC2_ACTIVE_MOD
.\mc2.exe
```

`MC2_ACTIVE_MOD` is read once, at process start (`mclib/file.cpp:668`) — setting or clearing it while `mc2.exe` is already running does nothing; you must restart the process.

### Option B — the launcher picker (recommended for players)

`tools/mc2_launcher/` builds a small picker EXE that scans `<its own folder>/mods/*/mod.json`, lists every `type: "campaign"` entry plus a "Base Game" option, and launches `mc2.exe` (expected next to the launcher) with `MC2_ACTIVE_MOD` set (or cleared, for "Base Game") in the child process's environment (`tools/mc2_launcher/mc2_launcher.cpp:117-140`). If the launcher finds zero mods in `mods/`, it skips the picker UI entirely and launches straight into base game mode.

Drop your mod folder into `<game-folder>/mods/`, run the launcher, and pick it from the list.

## 5. Packaging and installing with `mc2mod.py`

`tools/mc2mod/mc2mod.py` is the packager/installer for sharing a mod as a single file.

```powershell
# Pack a mod project directory into <id>-<version>.mc2mod (zip)
py -3 tools/mc2mod/mc2mod.py pack <mod-dir> --out <output-dir>

# Verify a package's integrity (re-hashes every member against its manifest)
py -3 tools/mc2mod/mc2mod.py verify-lite <package>.mc2mod

# Install into a deploy directory: extracts to <deploy>/mods/<id>/,
# writes <deploy>/mods/<id>/.install-receipt.json
py -3 tools/mc2mod/mc2mod.py install <package>.mc2mod --deploy <your-deploy-dir>

# Uninstall (uses the install receipt as ground truth; refuses if files were
# hand-edited since install unless you pass --force)
py -3 tools/mc2mod/mc2mod.py uninstall <id> --deploy <your-deploy-dir> [--force]
```

**`install` refuses to write into the project's canonical dev deploy roots** (hard-coded in `tools/mc2mod/mc2mod.py:55-58`, currently `a:/games/mc2-opengl/mc2-win64-v0.4` and `a:/games/mc2-opengl/mc2-win64-0.4c`). This is a safety rail for this repository's own development lanes, not a general restriction — **point `--deploy` at your own copy of the game folder** (wherever you personally extracted/installed it) and it will work normally. If you get `ERROR: refusing to install into canonical deploy root`, you've pointed `--deploy` at one of those two specific dev paths; use a different folder.

`version` in `mod.json` is optional and defaults to `"0.0.0"` if omitted — the package filename becomes `<id>-<version>.mc2mod`.

## 6. Diagnostics

| Env var | Values | Effect |
|---|---|---|
| `MC2_LOG_FILE_RESOLVE` | unset/empty = off; `1` = level-1 stdout tags; `2` = level-1 + JSONL trace file; any other non-numeric value is treated as `1` for backward compatibility | Prints `[mod] ...` lines: load order, per-layer file counts, and `[mod-dup]` lines whenever two mounted layers ship the same relative path (loser is named) |
| `MC2_RESOLVE_TRACE_FILE` | file path | Only consulted when `MC2_LOG_FILE_RESOLVE=2`. Appends one JSON line per file-resolution attempt: `{"key", "req", "layer", "path", "shadowed"}` where `layer` is one of `mod:<id>`, `base-loose`, `base-strip`, `fastfile`, `cd`, or `MISS` |
| `MC2_REBUILD_MOD_CACHE` | any non-empty value | Forces a fresh directory scan instead of trusting `mods/<id>/.modindex-cache` |

### The stale-cache trap

Each mod layer gets a cache file at `mods/<id>/.modindex-cache` (`mclib/file.cpp:281`) so a repeat launch does one sequential file read instead of rescanning the whole tree. The cache is invalidated by comparing its own mtime against the mtime of `data/` **and its immediate + one-more-level subdirectories only** (`GetModDataMtime`, `mclib/file.cpp:298-333`) — a two-level sweep, not a full recursive walk.

Practical consequence: if you add or remove a file **three or more directory levels below `data/`** (e.g. `data/tgl/mymech/textures/newfile.tga` — `textures` is level 3), the directory whose mtime actually changes (`textures`) isn't one the cache checks, so the mod may still launch with the stale file list until the cache is forced to rebuild. **Editing the contents of a file that's already in the cache is always safe** — the cache only stores *paths*, so content changes are picked up fresh on every launch regardless.

If your mod isn't picking up a change you just made:

```powershell
$env:MC2_REBUILD_MOD_CACHE = "1"
$env:MC2_LOG_FILE_RESOLVE = "1"
.\mc2.exe
Remove-Item Env:MC2_REBUILD_MOD_CACHE
```

Watch stdout for `[mod-cache] hit modid='...'` (cache was trusted) vs no such line (fresh scan ran) and for `[mod] [1/N] active '<id>': <count> files` to confirm your file count looks right.

## 7. Hard boundary: only `data/` is moddable

The overlay mechanism only indexes `mods/<id>/data/` and only shadows paths under the base game's `data/` tree. **`shaders/` and the `mc2.exe` binary itself are not moddable through this system** — there is no shader- or code-overlay path. If your idea requires changing a shader or engine behavior, that's engine-source modding (see [`docs/modding-guide.md`](../../modding-guide.md)), which needs a full rebuild and cannot ship as a `.mc2mod` package.

## 8. What's next

See [`mechanism-map.md`](mechanism-map.md) in this same folder for a verified reference table of every specific data-modding mechanism (weapons, mechs, buildings/trees, etc.) with exact source-file anchors — use it to find the entry point for whatever you're trying to change, then check the worked examples alongside this chapter for step-by-step walkthroughs.
