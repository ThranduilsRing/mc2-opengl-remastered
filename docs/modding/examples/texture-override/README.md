<!-- doc-status: current | reviewed:2026-10-11 -->
# Example Mod: Texture + Sound Override ("Hello World")

> **AVAILABILITY**
> - **Works on:** this checkout — the texture half is a plain data-mod override using the mechanism documented in [`../getting-started.md`](../getting-started.md) (`mclib/file.cpp`'s mod-overlay, `tools/mc2mod/mc2mod.py`). No branch requirements, no rebuild.
> - **Env var:** `MC2_ACTIVE_MOD` — unset by default. Set it to `hello-texture-sound` (this mod's **folder name** under `mods/`) to activate it.
> - **Correction to `../getting-started.md`'s `id` field table:** that page says `id` "is the value you pass to `MC2_ACTIVE_MOD`." That's imprecise — the engine's `InitModSearchPaths` (`mclib/file.cpp`, from line 632) uses `MC2_ACTIVE_MOD`'s value **literally as the `mods/<...>` subfolder name** it opens (`mods/<value>/mod.json`, `mods/<value>/data/`); `mod.json`'s `id` field is read only afterward, purely as a cosmetic label for log lines (`mclib/file.cpp:679`). If your folder name and `mod.json`'s `id` ever diverge, set `MC2_ACTIVE_MOD` to the **folder name** — not the `id` — or the mod silently fails to load (`no mod.json for ...`, zero files indexed). This tutorial's own folder name (`hello-texture-sound`) happens to equal its `id`, which is why the two are used interchangeably below.
> - **The sound half works differently than you'd expect** — read [Part 2](#part-2-sound-override-and-its-real-limitation) before you go looking for a missing feature. This is a verified, cited engine behavior in *this* checkout, not a "coming soon."
> - No build required. Everything here is data-only against a compiled `mc2.exe` you already have.

**Read [`../getting-started.md`](../getting-started.md) first** if you haven't set up a mod folder before — it explains `MC2_ACTIVE_MOD`, the `mods/<id>/data/...` folder shape, and the launcher picker. This page assumes you know that already. For the full catalogue of what else you can override, see [`../mechanism-map.md`](../mechanism-map.md).

## What this mod does

Two overrides, both shipped the same way — a whole-file drop-in under `mod/data/...` that shadows a file the base game already has:

| Asset | Overridden path | What changes |
|---|---|---|
| Texture | `data/art/cursors1.tga` | The mouse-cursor sprite sheet (walk/run/attack/etc. icons) is recolored green |
| Sound | `data/sound/music27.wav` | The main-menu music track is swapped for a synthesized chime — **but see Part 2, this one doesn't actually take effect in-game in this checkout** |

Payload layout (mirrors a real `mods/<id>/` folder):

```
mod/
  mod.json                       <- id/name/version the launcher and mc2mod read
  data/
    art/
      cursors1.tga                <- shadows data/art/cursors1.tga
    sound/
      music27.wav                 <- shadows data/sound/music27.wav
  source/                         <- NOT read by the game; how the two files above were made
    cursors1_stock.tga            <- a copy of the real stock texture, for reproducibility
    recolor_cursor.py             <- Pillow script: stock texture -> green-tinted mod texture
    make_chime.py                 <- stdlib script: synthesizes the chime .wav from scratch
```

## Part 1: texture override (this one works exactly as documented)

### How `data/art/cursors1.tga` was picked and verified

This isn't an arbitrary texture — it was traced end-to-end through the source so the "what you should see" step below is a sure thing, not a guess:

1. Every generic UI button's click sound and the mouse cursor sprites are set up in `mclib/userinput.cpp`. `UserInput::initMouseCursors("cursors")` is called at real call sites — `code/loadscreen.cpp:535`, `code/mainmenu.cpp:196`, `code/mechcmd2.cpp:1083` — so this path runs every time the game boots into the main menu or a mission, not just in some edge case.
2. `initMouseCursors` opens `data/defs/ui/legacy/shared/cursors.fit` (via `artPath` + `"cursors" + ".fit"`) and reads a block per cursor state. `[Cursor0]` ("walk", the default move cursor) has `FileName = "Cursors1"`.
3. `StaticInfo::init` (`mclib/utilities.cpp:259-303`) turns that into a full path: `FullPathFileName fullPath; fullPath.init(artPath, fileName, ".tga")` — lowercased, so the final string is `data/art/cursors1.tga`. It's then opened via `mcTextureManager->loadTexture()`, which ultimately calls `mclib/file.cpp`'s `File::open()` — the same mod-aware resolver documented in `getting-started.md` (checks the active mod's `data/` tree first, then base loose files, then packed FastFiles).
4. Confirmed the base game actually ships this file **packed inside `art.fst`** (not as a loose file) — `data/art/cursors1.tga` was found at byte offset 2959 of `art.fst` in a stock v0.4 install. That's a good illustration of the mod-overlay priority ladder in action: a loose file in your mod's `data/` folder outranks a FastFile-packed original.
5. The actual stock pixels (128x128, RGBA, alpha-keyed at `(255,0,255,0)`) were pulled from `art/cursors1.tga` in the `mc2-srcdata` sibling checkout (the pre-cook source tree) and are included here at `mod/source/cursors1_stock.tga` for reproducibility.

### Step 1 — get a copy of the mod folder

Copy the `mod/` folder from this tutorial into your own game's `mods/` directory and rename it to the mod id:

```powershell
# Run from wherever you keep this docs repo checked out
Copy-Item -Recurse "docs/modding/examples/texture-override/mod" "<your-game-folder>/mods/hello-texture-sound"
```

`<your-game-folder>` is wherever you personally have `mc2.exe` installed — **not** this repo's own dev deploy roots.

### Step 2 — regenerate the texture yourself (optional, but this is the actual lesson)

The shipped `mod/data/art/cursors1.tga` was produced by running the recolor script once. To see the whole pipeline yourself:

```powershell
py -3 -m pip install Pillow   # if you don't already have it
cd docs/modding/examples/texture-override/mod/source
py -3 recolor_cursor.py
```

Expected output:

```
wrote <...>\mod\data\art\cursors1.tga (128x128, RGBA)
```

Open `mod/source/cursors1_stock.tga` next to `mod/data/art/cursors1.tga` in any image viewer — the stock sheet is white/grey/blue line art; the mod version is green. Fully transparent pixels (the color-keyed background between icons) are left completely untouched by the script, which matters here: `mclib/mouse.cpp` comments warn cursor art "must NOT be anything BUT keyed" — recoloring the transparent pixels would break the cursor's shape.

### Step 3 — activate the mod and launch

```powershell
cd <your-game-folder>
$env:MC2_ACTIVE_MOD = "hello-texture-sound"
.\mc2.exe
```

(Or use the launcher picker — drop the mod folder in `mods/`, run `tools/mc2_launcher`'s built EXE, and pick "Hello World: Texture + Sound Override" from the list.)

### What you should see

At the main menu (or in a mission), move your mouse over the terrain/UI. The default move cursor — and every other cursor state (attack, run, jump, repair, etc.), since they all share the one sprite sheet — is tinted green instead of its stock white/grey/blue. It's a small sprite, so look closely; the shape and hotspot are identical, only the color changed.

To go back to the base game in the same shell:

```powershell
Remove-Item Env:MC2_ACTIVE_MOD
.\mc2.exe
```

### Troubleshooting

| Symptom | Try this |
|---|---|
| Cursor still looks stock | Confirm the mod folder is at `<game-folder>/mods/hello-texture-sound/` (not nested one level too deep) and that `data/art/cursors1.tga` exists under it |
| Not sure the mod loaded at all | `$env:MC2_LOG_FILE_RESOLVE = "1"; .\mc2.exe` — watch stdout for `[mod] [1/N] active 'hello-texture-sound': N files` and `[mod-hit] ... data/art/cursors1.tga -> ...` |
| You edited the mod's files and nothing changed | `$env:MC2_REBUILD_MOD_CACHE = "1"` once, then unset it — see the stale-cache trap in `getting-started.md` §6 (only matters if you *add or remove* files 3+ levels under `data/`; editing existing file contents is always picked up fresh) |

## Part 2: sound override — and its real limitation

This section documents something genuinely useful: **not every asset type under `data/` is shadowed the same way.** The texture above goes through the engine's general-purpose file resolver. Streamed music/dialogue `.wav` files, as shipped in this checkout, do not.

### How the main-menu music was traced

1. `code/missionbegin.cpp:233-246` builds the main menu screen by opening `data/art/mcl_mm.fit` (via `file.open(path)` — the same mod-aware `File::open`) and calling `mainMenu->init(file)`.
2. `mcl_mm.fit` has `l TuneId = 27` (confirmed identical in both the `mc2-srcdata` source tree and packed inside the shipped `art.fst`).
3. `code/mainmenu.cpp:465-469`, in `MainMenu::update()`, does `soundSystem->playDigitalMusic(tuneId)` the first time the menu updates — i.e. this always fires as soon as the main menu appears.
4. `data/sound/sound.snd`'s `[DigitalMusic]` block maps slot 27 to a filename: `st DMS27 = "music27"  // "Bandits444s" - Splash Screen Loop`, `b DMSLoop27 = t` (it loops). So the file that plays is `data/sound/music27.wav`, continuously, from the moment the main menu appears.
5. **Here's the catch.** `SoundSystem::playDigitalMusic` (`mclib/soundsys.cpp:716-819`) builds the path with `FullPathFileName digitalStream; digitalStream.init(soundPath, digitalMusicIds[musicId], ".wav")` — a plain string `"data/sound/music27.wav"` — and:
   - First calls `fileExists(digitalStream)` (`mclib/soundsys.cpp:738`). `fileExists()` (`mclib/file.cpp:776-797`) **does** check the active mod's overlay first via `TryModOpen`, so this check honestly reports "yes, this path exists" whether it's the mod's copy or the base copy.
   - Then calls `gosAudio_CreateResource(&stream1Handle, gosAudio_StreamedFile, (const char *) digitalStream)` (`mclib/soundsys.cpp:748`). This resource type is implemented in `GameOS/gameos/gameos_sound.cpp:253-292` with `SDL_LoadWAV(file_name, &spec, &databuf, &src_datasize)` (or `Mix_LoadWAV(file_name)` under `USE_MIX_LOAD`) — **a raw OS file open of the literal string, with no call into `mclib/file.cpp`'s mod resolver at all.**
   - Every call site of `gosAudio_StreamedFile` in the engine (`mclib/soundsys.cpp:748,767,792,850`) passes a plain relative path this same way. There is no code path where streamed music/dialogue audio is read through the mod-aware `File` class.

Net effect: the *presence check* is mod-aware, but the *actual audio bytes* are loaded by a second, independent step that isn't. A mod's `data/sound/music27.wav` is correctly detected as "there," but the audio device never actually gets those bytes — it gets whatever `data/sound/music27.wav` resolves to relative to the game's own install folder (the base game's copy), because `SDL_LoadWAV`/`Mix_LoadWAV` know nothing about `mods/<id>/`.

This is a real, current limitation of this checkout's audio subsystem — not a mistake in how this tutorial's mod is built.

### Why the shipped chime is synthesized, not a recolored copy

The real stock `data/sound/music27.wav` is Microsoft ADPCM-encoded (Python's stdlib `wave` module refuses to even read it: `wave.Error: unknown format: 2`, confirmed against both the `mc2-srcdata` source copy and the deployed v0.4 copy). Decoding/re-encoding ADPCM correctly needs a real audio codec, which is out of scope for a "tiny script." `mod/source/make_chime.py` instead synthesizes a short 3-note PCM16 mono 44100Hz chime from scratch using only `wave`/`math`/`struct` — that format matches what `sound.snd`'s `[DigitalMusic]` block declares for the stream (`StreamBitDepth=16`, `StreamChannels=1`).

```powershell
cd docs/modding/examples/texture-override/mod/source
py -3 make_chime.py
```

Expected output:

```
wrote <...>\mod\data\sound\music27.wav (0.90s, 44100Hz, mono, 16-bit PCM)
```

### Proving the limitation to yourself (optional)

With the mod active and `MC2_LOG_FILE_RESOLVE=1`, watch for a line like:

```
[mod-hit] [hello-texture-sound] data/sound/music27.wav -> <your-game-folder>\mods\hello-texture-sound\data\sound\music27.wav
```

That line proves the resolver found and would happily serve your mod's file. Despite that, the main-menu music you actually hear is unchanged — because the code path that plays it never asks the resolver in the first place (see the trace above).

### If you actually want to hear a different main-menu tune today

The only thing that works in this checkout is replacing the base game's own loose file directly — **not** through the mod-overlay system:

```powershell
# Back up the original first!
Copy-Item "<your-game-folder>\data\sound\music27.wav" "<your-game-folder>\data\sound\music27.wav.bak"
Copy-Item "mod\data\sound\music27.wav" "<your-game-folder>\data\sound\music27.wav"
```

This is a direct edit of your own install, not a shareable, uninstallable mod — treat it as a personal experiment, and keep the `.bak` so you can restore the stock track.

## Level 2: packaging with `mc2mod.py`

Once your `mod/` folder looks right, package it the same way any other data mod is shared. All commands below were actually run against this exact payload; the transcripts are real, not illustrative — including the sha256 values, which are reproducible (see the callout below).

### Pack

```powershell
py -3 tools/mc2mod/mc2mod.py pack docs/modding/examples/texture-override/mod --out docs/modding/examples/texture-override/dist
```

```
pack: A:\Games\mc2-modding-docs\docs\modding\examples\texture-override\dist\hello-texture-sound-1.0.0.mc2mod
  id=hello-texture-sound  version=1.0.0  files=6  sha256=10a366362bfd9e2c34e9fd448a949e9b6b5ca3130c5598112c3adfcd225caccd
```

(6 files = `mod.json` + the 2 shadowed data files + the 3 files under `source/` — `pack` only excludes top-level `src/`/`out/` and dot-prefixed entries, not `source/`, so the authoring scripts ship inside the package too. That's harmless and matches the existing `building-import` example in this same folder.)

> **Your sha256 should match the one above exactly.** `pack` is reproducible on this checkout: every zip member's timestamp and permissions are pinned (`_write_zip_member` / `_REPRO_ZIP_DATE` in `tools/mc2mod/mc2mod.py`), so a byte-identical `mod/` folder always packs to byte-identical package bytes. If your hash differs, your payload differs from this folder's — diff `mod/` before suspecting the tool. (An earlier version of this doc — and of `mc2mod.py` — had genuinely non-reproducible hashes: the generated `package.json` zip entry was stamped with wall-clock time and payload entries inherited source mtimes, so every `pack` run produced a new hash. That gap was fixed upstream by pinning the `ZipInfo` metadata; the shipped `dist/hello-texture-sound-1.0.0.mc2mod` was re-packed with the fixed writer so it matches a fresh pack of this payload.) On any checkout, `verify-lite` reporting `PASS` remains the integrity verdict that matters.

### Verify-lite

```powershell
py -3 tools/mc2mod/mc2mod.py verify-lite docs/modding/examples/texture-override/dist/hello-texture-sound-1.0.0.mc2mod
```

```
verify-lite PASS: id=hello-texture-sound  version=1.0.0  files=6  pkg_sha256=10a366362bfd9e2c34e9fd448a949e9b6b5ca3130c5598112c3adfcd225caccd
```

`pkg_sha256` here is just the sha256 of *your* freshly built `.mc2mod` file, reported back to you — it will equal whatever `pack` printed for that same file a moment ago, and (packing this exact payload on this checkout) the value printed in this doc too. `PASS` is the signal that matters.

### Install (into your own game folder — never a canonical dev deploy root)

```powershell
py -3 tools/mc2mod/mc2mod.py install docs/modding/examples/texture-override/dist/hello-texture-sound-1.0.0.mc2mod --deploy "<your-game-folder>"
```

Verified against a scratch directory during authoring — real transcript:

```
A:\Games\mc2-modding-docs\tools\mc2mod\mc2mod.py:427: DeprecationWarning: datetime.datetime.utcnow() is deprecated and scheduled for removal in a future version. Use timezone-aware objects to represent datetimes in UTC: datetime.datetime.now(datetime.UTC).
  install_time = datetime.datetime.utcnow().strftime("%Y-%m-%dT%H:%M:%SZ")
install: hello-texture-sound v1.0.0 -> <your-game-folder>\mods\hello-texture-sound
  files installed : 6
  receipt         : <your-game-folder>\mods\hello-texture-sound\.install-receipt.json

=== MATCHED-TUPLE ADVISORY ===
  exe_sha256     : ABSENT (<your-game-folder>\mc2.exe)
  shaders        : ABSENT (<your-game-folder>\shaders)
  active_mod_id  : hello-texture-sound
  package_sha256 : 361ac9262056b0a83b3f9cb84950e4a6775d4881a12e013ac390be184f38ecc1
  registry_index : absent
==============================
```

Both of these are expected and harmless, not signs anything broke:
- The `DeprecationWarning` about `datetime.datetime.utcnow()` is a Python 3.12+ warning from `mc2mod.py` itself (line 427); it doesn't affect the install.
- The `MATCHED-TUPLE ADVISORY` block always prints after a successful install — it's `mc2mod.py`'s informational summary of what it found at your deploy target (here, `ABSENT`/`absent` because the scratch directory used for this run had no real `mc2.exe`/`shaders`/registry — against your actual game folder these fields will be populated).

It extracts to `<deploy>/mods/hello-texture-sound/` and writes a `.install-receipt.json` next to it, exactly as documented in `getting-started.md` §5.

**Canonical deploy root guard — read the exact strings, not the shorthand.** This repo's installer refuses to write to two exact, hardcoded paths (`tools/mc2mod/mc2mod.py`, `_CANONICAL_DEPLOY_ROOTS`):

- `a:/games/mc2-opengl/mc2-win64-v0.4`
- `a:/games/mc2-opengl/mc2-win64-0.4c` (no `v` before `0.4c`)

Only those two exact strings are blocked. **`a:/games/mc2-opengl/mc2-win64-v0.4c` (with a `v`) is a different string and is *not* guarded** — it's a real, currently-active project dev deploy lane, and `install` will happily write into it if you point `--deploy` there. Don't construct a path by pattern-matching the first example above; use your own game install folder (wherever *you* personally put `mc2.exe`), not any path under this repo's own `a:/games/mc2-opengl/` tree.

### Uninstall

```powershell
py -3 tools/mc2mod/mc2mod.py uninstall hello-texture-sound --deploy "<your-game-folder>"
```

Removes the installed files and the now-empty directories using the install receipt as ground truth.

## Summary

- **Texture overrides** (and by extension CSVs, `.fit` files, JSON manifests, glTF meshes — anything read through `mclib/file.cpp`'s `File::open`) follow the whole-file shadow mechanism exactly as documented in `getting-started.md`.
- **Streamed music/dialogue `.wav` overrides do not work via `MC2_ACTIVE_MOD` in this checkout** — verified via source trace, not assumption. If you need this to work for a real mod, that's a genuine engine gap worth flagging upstream rather than something to work around with clever data-mod tricks.
