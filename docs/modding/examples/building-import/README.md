<!-- doc-status: current | reviewed:2026-10-11 -->
# Example Mod: Replace a Building's Model (`building-import`)

Swap the stock **hangar** building for your own glTF model — render only,
no gameplay/collision change — using MC2's static-prop **model override
registry**.

> **AVAILABILITY**
> - **Works on:** this checkout, branch `claude/modding-examples-1` (`mclib/model_override_registry.{h,cpp}`, resolve seams in `mclib/bdactor.cpp:357` (staticProp) and `:4106` (tree)). Verified present and committed (`git log -1 -- mclib/model_override_registry.cpp` → `2eddfcf9`).
> - **Entry point:** `data/model_overrides/models.json`, parsed by `ModelOverrideRegistry::instance()` (`mclib/model_override_registry.cpp:158-166`).
> - **No env var turns this mechanism on or off** — it is always active; an empty/missing `models.json` is a safe no-op (0 overrides, stock renders).
> - **Diagnostics:** `MC2_MODOVERRIDE_TRACE=1` (discovery + apply trace, see §3). `MC2_ASSIMP_TRACE=1` (glTF import detail). Both unset by default.
> - **Build:** no rebuild needed — `ENABLE_ASSIMP_IMPORTER` defaults `ON` (`CMakeLists.txt:204`) on a stock build.
> - ⚠️ **This mechanism does *not* go through the `MC2_ACTIVE_MOD` overlay described in [`../getting-started.md`](../getting-started.md).** See §2 — it is a **critical, verified exception** to that chapter's whole-file-shadow model. Read §2 before you do anything else in this doc.

If you haven't read [`../getting-started.md`](../getting-started.md) (mod folder anatomy, `MC2_ACTIVE_MOD`, the launcher, `mc2mod.py` lifecycle) and [`../mechanism-map.md`](../mechanism-map.md) (reference table for every mechanism), do that first — this page only covers what's specific to building/prop model overrides.

**Props = this pipeline.** There is exactly one data-driven way to replace a static prop or tree's *visual* mesh in this checkout: the model override registry described here. It is not gated behind an experimental flag and is not on an unmerged branch — it is the real, shipped mechanism (the stock game itself already ships tree overrides through it — see §2).

---

## 1. What you'll build

A tutorial-sized mod that replaces the stock **hangar** building (`data/tgl/hangar.ini`, appearance name `hangar`) with a simple textured box, so the change is unmistakable in-game. The box reuses the stock `a_hangar` texture tile that already ships with the game — no new texture asset needed.

You will see: every hangar building in the mission (e.g. `mc2_01`) becomes a plain box the same footprint/height as before. Collision, pathing, and the minimap footprint are **unchanged** — only the drawn mesh changes (this is "render-only": see §5).

## 2. CRITICAL — this file bypasses the mod overlay. Verified, not assumed.

[`../getting-started.md`](../getting-started.md) describes a whole-file-shadow overlay: files under `mods/<id>/data/...` take priority over `data/...`, resolved through `mclib/file.cpp`'s `File::open` → `TryModOpen` → `g_modIndex` lookup (built from `MC2_ACTIVE_MOD` at process start).

**`data/model_overrides/models.json` never goes through that path.** The registry singleton loads it with a hardcoded literal and a raw `std::ifstream`:

```cpp
// mclib/model_override_registry.cpp:158-166
g.loadFromFile("data/model_overrides/models.json", "data/model_overrides");
```

`std::ifstream` opens a path relative to the process's current working directory — it never calls `File::open`, `TryModOpen`, or consults `g_modIndex`. The glTF import that follows a resolved override (`Assimp::Importer::ReadFile`, `mclib/assimp_importer.cpp:479`) is the same story — Assimp reads straight off disk. **Neither the manifest nor the override source model is mod-overlay-aware.** Setting `MC2_ACTIVE_MOD` has **zero effect** on this file, even though it affects almost everything else in this collection.

Practical consequences:

1. **You must edit the real, deployed `<game-folder>/data/model_overrides/models.json` directly.** Dropping your `models.json` into `mods/<id>/data/model_overrides/models.json` and setting `MC2_ACTIVE_MOD` does **nothing** — the file at that path is simply never opened.
2. **It's a single shared file, not a shadow layer.** If `data/model_overrides/models.json` already exists on your install (this checkout's own `data/model_overrides/models.json` already ships 7 live tree overrides — real production content, not a fixture), you must **hand-merge** your entry into its `"overrides"` array. Overwriting the whole file would silently delete every other override already present. There is no dependency-chain merging here the way `mod.json` does for the real overlay.
3. **`tools/mc2mod/mc2mod.py install` cannot deploy this correctly** — verified below (§4.3). It always extracts to `<deploy>/mods/<id>/...`, which this file never reads from.

## 3. Discover a real appearance name with `MC2_MODOVERRIDE_TRACE`

You need a stock building's `appearanceName` — the string after the `:` in `"replaces": "staticProp:<name>"`. Don't guess it; discover it:

```powershell
cd <your-game-folder>
$env:MC2_MODOVERRIDE_TRACE = "1"
.\mc2.exe -mission mc2_01
```

Watch stderr (or the log if your build redirects it) for lines like:

```
[MODOVERRIDE_TRACE] staticProp 'hangar'
[MODOVERRIDE_TRACE] staticProp 'quonset2'
[MODOVERRIDE_TRACE] staticProp 'sandbagbunker'
...
```

This fires once per static-prop appearance actually loaded by the mission, **whether or not an override exists for it** (`mclib/bdactor.cpp:354`) — it's a full inventory, not just hits. `hangar` comes from `data/tgl/hangar.ini`'s `FileName0="hangar"` key (`mclib/bdactor.cpp:320/338` captures this into `bldgBaseName` before the resolve call) — confirmed present in the deploy tree at `A:/Games/mc2-opengl/mc2-win64-v0.4/data/tgl/hangar.ini`. Tree appearance names come from the equivalent `[MODOVERRIDE_TRACE] tree '<name>'` line.

```powershell
Remove-Item Env:MC2_MODOVERRIDE_TRACE
```

## 4. Install the example

### 4.1 What's in this folder

```
mod/
  mod.json                              <- packaging metadata only (see §4.3) — NOT read by the override mechanism
  data/
    model_overrides/
      models.json                       <- the ONE file that matters at runtime — must be merged into your install's copy
      cooked/hangar_demo_box/
        hangar_demo_box.glb             <- the model actually loaded (referenced by models.json's "source")
        manifest.json                   <- asset-cook manifest (provenance/validator input, not read at runtime)
        materials.json                  <- material fragment used to author manifest.json (not read at runtime)
        models.generated.json           <- the models.json fragment this asset would contribute (not read at runtime)
        staged.json                     <- intermediate cook-pipeline staging record (not read at runtime)
      source/
        hangar_demo_box.glb             <- pre-cook authoring copy, byte-identical to the cooked one here (kept for reference)
  tools/
    make_placeholder_glb.py             <- regenerates the box glTF from scratch (pure stdlib, no Blender needed)
```

Only two files are load-bearing at runtime: `data/model_overrides/models.json` and `data/model_overrides/cooked/hangar_demo_box/hangar_demo_box.glb`. Everything else is authoring/provenance/tooling.

### 4.2 Merge `models.json` into your install (manual copy — not `mc2mod install`)

```powershell
$game = "<path-to-your-game-folder>"   # e.g. a copy of mc2-win64-v0.4, NOT the project's canonical dev deploy roots
New-Item -ItemType Directory -Force "$game\data\model_overrides\cooked\hangar_demo_box" | Out-Null
Copy-Item ".\mod\data\model_overrides\cooked\hangar_demo_box\hangar_demo_box.glb" "$game\data\model_overrides\cooked\hangar_demo_box\hangar_demo_box.glb"
```

Now merge the override entry into `$game\data\model_overrides\models.json`:

- **If that file doesn't exist yet** (a stock, unmodified deploy — confirmed absent on `A:/Games/mc2-opengl/mc2-win64-v0.4` at the time of writing), just copy ours in:
  ```powershell
  Copy-Item ".\mod\data\model_overrides\models.json" "$game\data\model_overrides\models.json"
  ```
- **If it already exists** (this checkout's own `data/model_overrides/models.json` ships live tree overrides — see §2), open it and append this object into its top-level `"overrides"` array by hand. Before/after example, given a file that starts with one existing entry:

  **Before:**
  ```json
  {
    "overrides": [
      { "type":"model","class":"tree","replaces":"tree:pine01","source":"cooked/pine01/pine01.glb","renderOnly":true,"scale":1.0,"fallback":"stock" }
    ]
  }
  ```

  **After** (note the comma added after the first entry's closing `}`):
  ```json
  {
    "overrides": [
      { "type":"model","class":"tree","replaces":"tree:pine01","source":"cooked/pine01/pine01.glb","renderOnly":true,"scale":1.0,"fallback":"stock" },
      { "type":"model","class":"staticprop","replaces":"staticprop:hangar","source":"cooked/hangar_demo_box/hangar_demo_box.glb","renderOnly":true,"scale":1.0,"fallback":"stock" }
    ]
  }
  ```

  Just the new entry, for copy-paste:
  ```json
  { "type":"model","class":"staticprop","replaces":"staticprop:hangar","source":"cooked/hangar_demo_box/hangar_demo_box.glb","renderOnly":true,"scale":1.0,"fallback":"stock" }
  ```

**Optional — only needed if you plan to run §4.4's `--check-files` validation against this merged install.** The engine itself never reads `data/model_overrides/source/` (§4.1 marks it "kept for reference"), so skip this if you're only installing the mod to play it. But `manifest.json`'s `geometry.source` field (an authoring/provenance pointer, not a runtime path) still gets checked by the validator's `--check-files` flag whenever it's pointed at a `--deploy-root`, so if you want that check to pass, copy the source file in too:
```powershell
New-Item -ItemType Directory -Force "$game\data\model_overrides\source" | Out-Null
Copy-Item ".\mod\data\model_overrides\source\hangar_demo_box.glb" "$game\data\model_overrides\source\hangar_demo_box.glb"
```

### 4.3 `mc2mod.py` — pack/verify-lite work, `install` does NOT (verified)

`tools/mc2mod/mc2mod.py pack` and `verify-lite` are format-agnostic (they just zip/hash a directory), so they work fine on this payload:

```
py -3 tools/mc2mod/mc2mod.py pack docs/modding/examples/building-import/mod --out <out-dir>
  -> pack: ...\building-import-0.1.0.mc2mod
     id=building-import  version=0.1.0  files=9  sha256=bc9df64f...

py -3 tools/mc2mod/mc2mod.py verify-lite <out-dir>\building-import-0.1.0.mc2mod
  -> verify-lite PASS: id=building-import  version=0.1.0  files=9  pkg_sha256=bc9df64f...
```

But **do not use `mc2mod.py install` to deploy this mod.** Verified by actually installing it into a scratch deploy directory: `install` unconditionally extracts to `<deploy>/mods/<id>/...` (`tools/mc2mod/mc2mod.py:401`), so the payload lands at `<deploy>/mods/building-import/data/model_overrides/models.json` — a path the engine **never reads** (§2). The install command reports success and writes a receipt, but the override has no effect in-game. If you package this as a `.mc2mod` for distribution, tell users to unzip it manually and copy `data/` on top of their install (§4.2's manual steps), not to run `mc2mod.py install`.

### 4.4 Validate the manifest

```powershell
py -3 tools/asset_cook/validate_asset_manifest.py docs/modding/examples/building-import/mod/data/model_overrides/cooked/hangar_demo_box/manifest.json
```
```
PASS manifest.json format=cook id=hangar_demo_box replaces=staticprop:hangar materials=1
```

`--check-files` cross-checks the manifest's `geometry.source`/texture paths against a real deploy tree (they're literal `data/...`-rooted paths, matching §2's finding). Run it with `--deploy-root` pointed at your merged install (after §4.2):

```powershell
py -3 tools/asset_cook/validate_asset_manifest.py docs/modding/examples/building-import/mod/data/model_overrides/cooked/hangar_demo_box/manifest.json --check-files --deploy-root "$game"
```

⚠️ **This will `FAIL` unless you also did §4.2's optional `source/` copy step.** Verified: with only `data/model_overrides/cooked/hangar_demo_box/hangar_demo_box.glb` + `models.json` merged in (i.e. §4.2's required steps only, no `source/`), this command reports:
```
FAIL manifest.json (1 error(s)):
  - geometry.source not found: <deploy>\data\model_overrides\source\hangar_demo_box.glb
```
That's `geometry.source` — the manifest's provenance pointer to the pre-cook authoring file — not the cooked `.glb` or the texture; the mod is **not broken**, it will still render correctly in-game (§6). It's purely an artifact of `--check-files` validating provenance fields as well as runtime ones. Do the optional `source/` copy from §4.2, then re-run the same command:
```
PASS manifest.json format=cook id=hangar_demo_box replaces=staticprop:hangar materials=1
```
Verified end-to-end in a scratch deploy: cooked `.glb` + `models.json` + `source/hangar_demo_box.glb` merged in, plus the real `data/tgl/128/a_hangar.ktx2` copied from `A:/Games/mc2-opengl/mc2-win64-v0.4` (a stock install already has this texture — you don't need to copy it in yourself) → `PASS`. Run with `--deploy-root` pointed only at `docs/modding/examples/building-import/mod` (which has `source/` but not the stock texture) and you'll correctly get one `materials[0].albedo_ktx2.128 not found` instead — expected, since this mod intentionally reuses a texture it doesn't ship (see §4.5).

### 4.5 Why no new texture

`data/model_overrides/cooked/hangar_demo_box/manifest.json`'s one material uses `textureName: "a_hangar"`, matching the glTF's embedded image name (`a_hangar.png`, see `mod/tools/make_placeholder_glb.py`). The importer derives the MC2 texture name from the image stem (`DeriveMC2TextureName`, `mclib/assimp_importer.cpp`) and loads it by that name through the normal texture manager — which finds the **stock** `data/tgl/128/a_hangar.tga`/`.ktx2` already shipped with the game, no new asset required. The `a_` prefix marks it alpha-tested (matches `alphaClass:1` in the manifest). This is a legitimate shortcut for a tutorial; a real mod would ship its own texture under `data/tgl/128/`.

## 5. What "render-only" means here (and why collision is safe)

The registry only ever populates a separate `bldgRenderShape`/`treeRenderShape` on the appearance *type* (`mclib/bdactor.cpp:357-388`). The original stock shape used for collision/footprint (`calcCellsCovered`, pathing, minimap) is loaded unconditionally beforehand and is **never replaced**. Your override can only ever change what's drawn — never where a mech can walk or shoot. `renderOnly` must be exactly `true` and `fallback` exactly `"stock"` in every entry, or the whole entry is dropped (`mclib/model_override_registry.cpp:63-68`) — these aren't optional flourishes, they're enforced invariants of the format. Likewise `scale` must be exactly `1.0` (MVP limitation) and `replaces` must be `"staticProp:<name>"` or `"tree:<name>"` — no other class is accepted.

## 6. Verify in-game

1. Complete §4.2 (or §4.3's manual-copy note) against your own deploy — never the project's canonical dev roots.
2. Launch the game normally (no `MC2_ACTIVE_MOD` needed — this isn't overlay content) and load a mission that places a hangar, e.g. `mc2_01`.
3. **What you should see:** every hangar building is now a plain textured box (same footprint) instead of the detailed stock hangar mesh. Mechs/units still path around it and collide with it exactly as before.
4. To confirm the override actually applied (vs. silently falling back to stock), run with the trace on and look for the apply line:
   ```powershell
   $env:MC2_MODOVERRIDE_TRACE = "1"
   .\mc2.exe -mission mc2_01
   ```
   ```
   [MODOVERRIDE] registry loaded: 1 override(s)          <- or 8, if merged into this checkout's shipped tree overrides
   [MODOVERRIDE_TRACE] staticProp 'hangar'
   [MODOVERRIDE] staticProp 'hangar': render override applied (data/model_overrides/cooked/hangar_demo_box/hangar_demo_box.glb)
   [MODOVERRIDE_TEX] slot=0 name='a_hangar.tga' -> gosHandle=<n> alpha=1
   ```

## 7. Troubleshooting

| Symptom | Likely cause | Check |
|---|---|---|
| Hangar looks unchanged | `models.json` never merged into the real `data/model_overrides/models.json` (§2/§4.2) — remember `MC2_ACTIVE_MOD`/`mods/<id>/` does nothing here | `MC2_MODOVERRIDE_TRACE=1`, look for `registry loaded: N override(s)` — if `N` is lower than expected, your entry didn't merge |
| `registry loaded: 0 override(s)` | `models.json` missing, malformed JSON, or missing the top-level `"overrides"` array | Console prints `[MODOVERRIDE] %s: missing 'overrides' array` or a JSON parse error with the path — check for a stray trailing comma |
| Trace shows the appearance name but never `render override applied` | Entry got silently dropped by a validation rule (§5) — wrong `class`, `scale != 1.0`, `renderOnly` not `true`, `fallback` not `"stock"`, or a duplicate `replaces` key (first one wins) | Console prints `[MODOVERRIDE] dropped '<replaces>': <reason>` (`mclib/model_override_registry.cpp:14`) at registry-load time |
| `import failed (...), using stock render` | `hangar_demo_box.glb` missing at the resolved path, or not valid glTF/GLB | `MC2_ASSIMP_TRACE=1` for the Assimp `ReadFile` error string; re-check the `source` path is relative to `data/model_overrides/` (manifest dir), not to your mod folder |
| Box renders untextured / wrong texture | Texture name derived from the glTF image doesn't match a real MC2 texture, or the stock texture isn't actually present on this install | `MC2_MODOVERRIDE_TEX]` trace line shows `NOT FOUND` — confirm `data/tgl/128/a_hangar.tga` (or `.ktx2`) exists in your deploy |
| Ran `mc2mod.py install` and nothing changed | Expected — see §4.3. `install` puts files under `mods/<id>/`, which this mechanism never reads | Re-do the manual copy in §4.2 instead |
| §4.4's `--check-files --deploy-root "$game"` reports `geometry.source not found` | You skipped §4.2's optional `source/hangar_demo_box.glb` copy step — harmless for actually playing the mod (the engine never reads `source/`, only `cooked/`), but the validator checks it too | Either do the optional copy step in §4.2, or ignore this specific error — it doesn't mean the mod is broken |

`MC2_LOG_FILE_RESOLVE` (the overlay's own diagnostic, per [`../getting-started.md`](../getting-started.md) §6) will **not** show anything for `models.json` or the override `.glb` — that's expected and confirms §2, not a bug in the trace.

## 8. Regenerating the placeholder model

The box is a tiny (2 KB) hand-built GLB, pure stdlib, no Blender required:

```powershell
py -3 docs/modding/examples/building-import/mod/tools/make_placeholder_glb.py out.glb
```

Its geometry convention (glTF Y in `[-HEIGHT, 0]`, material named `a_hangar`, `alphaMode: MASK`) is documented in the script's own docstring — read it before adapting this example to a different stock building.
