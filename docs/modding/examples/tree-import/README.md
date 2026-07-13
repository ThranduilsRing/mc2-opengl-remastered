# Example Mod — Tree Import (render-only model override)

> **AVAILABILITY**
> - **Works on:** this checkout, any branch — the render-only model-override registry (`mclib/model_override_registry.{h,cpp}`, `data/model_overrides/models.json`) and its **tree** resolve seam (`mclib/bdactor.cpp:4106`) are shipped code, not gated behind an env var and not branch-only. Verified present at the time this doc was written.
> - **⚠ CONFIRMED ENGINE BUG — the mod overlay does NOT apply to this mechanism.** `ModelOverrideRegistry::instance()` always loads the manifest from the hardcoded literal path `data/model_overrides/models.json` via a raw `std::ifstream` (`mclib/model_override_registry.cpp:39-45,158-166`) — it never goes through the mod-aware `TryModOpen`/`File::open` resolver (`mclib/file.cpp:545,907-925`) that every other moddable asset type uses. The referenced `.glb`/`.gltf` geometry has the same problem one layer down: it's opened by `Assimp::Importer::ReadFile(path, ...)` on the literal OS path (`mclib/assimp_importer.cpp:475-486`), also bypassing the overlay. **Net effect: a mod's own `mods/<id>/data/model_overrides/...` files are never read, active mod or not.** Only the literal `<game-folder>\data\model_overrides\...` path is ever consulted. §4 below documents the required workaround (place the payload at the base-game path directly) until this is fixed engine-side. This is not tree-specific — it affects the `staticProp` half of the mechanism identically.
> - **No gate / env var turns this mechanism on or off** — if a valid `tree:<name>` entry exists in the base game's `data/model_overrides/models.json`, it applies. `MC2_ACTIVE_MOD` has **no effect** on whether this specific mechanism fires (see bug notice above) — you may leave it set or unset for this example.
> - **Diagnostics env vars** (all optional, unset by default): `MC2_MODOVERRIDE_TRACE` (any value = log registry resolve attempts to stderr), `MC2_ASSIMP_TRACE` (any value = verbose glTF import log), `MC2_GLTF_AXIS` (`0`-`3`, default `0`), `MC2_GLTF_YOFF` (float, default `0.0`), `MC2_GLTF_GROUND` (`0`/`1`/`2`, default `2`).
> - **Compile-time requirement:** `ENABLE_ASSIMP_IMPORTER` must be `ON` (it is by default — `CMakeLists.txt:204`). No rebuild is needed on a stock build.
> - **⚠ LOD chains for tree overrides are mid-work, NOT usable yet.** The registry's JSON schema *parses* an optional `lods` array on a tree entry, but wiring it up hits an open bug (per-instance light-slot count multiplies with LOD count instead of staying bounded — see `docs/HANDOFF-model-override-lod.md`, `TREE-OVERRIDE-LOD-MVP-1`, status **STOP**). This example ships **single-LOD only** (no `lods` key) — the one configuration that's actually shipped and safe.

---

## 0. Before you start

This doc assumes you've already read [`../getting-started.md`](../getting-started.md) (mod folder anatomy, `MC2_ACTIVE_MOD`, the launcher picker, `mc2mod.py` packaging, diagnostics) and skimmed [`../mechanism-map.md`](../mechanism-map.md) (the "Buildings, props, trees" section in particular). This doc only covers what's specific to **tree** overrides.

## 1. What this example does

It replaces the stock **oak1** tree's *visuals only* with a tiny placeholder glTF mesh — a two-quad "leaf card" cross using the game's own stock `oak1` leaf texture — while leaving oak1's collision, footprint, and gameplay behavior completely untouched. It's the smallest possible tree-override mod: one JSON entry, one `.glb` file.

You will:
1. See where the mechanism lives and how a tree override differs from the static-prop override you may have already tried (chiefly: pivot handling).
2. Install the example mod into your own copy of the game and confirm it renders.
3. Learn where to point the placeholder machinery at your own tree art.

## 2. The mechanism, specific to trees

Both static props and trees share one registry (`ModelOverrideRegistry`, singleton-loaded from `data/model_overrides/models.json` at `mclib/model_override_registry.cpp:158-166`) and one JSON entry shape:

> ⚠ **Read the AVAILABILITY box above first.** That singleton load (and the geometry import right after a hit) reads literal OS paths and does **not** go through the mod-overlay resolver — a mod's own copy of this manifest is invisible to the engine today. Section 4 gives you the working-today path.

```json
{ "type": "model", "class": "tree", "replaces": "tree:oak1", "source": "<relative-path-to-glb>", "renderOnly": true, "scale": 1.0, "fallback": "stock" }
```

- `class` must be exactly `"staticProp"` or `"tree"` (case-insensitive — the registry lowercases everything via `normalizeKey`, `model_override_registry.cpp:19-24`) and must match the class named in `replaces` (`:77-80`, entries that disagree are dropped and logged).
- `replaces` is `"<class>:<appearanceName>"` — for trees, `<appearanceName>` is the tree's `.ase` base name as declared by `FileName0` in the tree's `.ini` (see §3 below for how to find it).
- `type` must be exactly `"model"`, `renderOnly` must be exactly `true`, `fallback` must be exactly `"stock"`, and `scale` must be exactly `1.0` — any other value silently drops the whole entry (`:63-68`, MVP limitation, logged via `logDrop`).
- `source` must be a relative path ending `.glb` or `.gltf`, with no leading slash/drive letter and no `..` component (`isSafeSource`, `:27-34`) — it resolves relative to `data/model_overrides/` (the registry's `manifestDir_`), **not** relative to your mod's root.

**Resolve seam** — where the engine actually asks the registry for a tree override, at load time, per tree *type* (not per instance): `mclib/bdactor.cpp:4100-4106`, right after the stock `.ase` load populates the collision-authoritative `treeShape[0]`. On a hit it loads the override geometry into a **separate** `treeRenderShape[0]` (`:4113-4149`) — collision keeps reading the untouched stock shape; only what's drawn changes.

### The pivot difference vs. static props (the thing to actually learn here)

Look at the two call sites side by side:

```cpp
// staticProp (mclib/bdactor.cpp:374)
long r = ImportGeometryFromFile(overridePath, bldgRenderShape, /*autoGround=*/false);

// tree (mclib/bdactor.cpp:4123)
long r = ImportGeometryFromFile(overridePath, treeRenderShape[0]);
```

The tree call omits the third argument, so it takes the header default: `bool autoGround = true` (`mclib/assimp_importer.h:38`).

What `autoGround` actually does, after the mesh is fully imported and axis-mapped into MC2 space (`mclib/assimp_importer.cpp:539-555`):

```cpp
// AUTO-GROUND: imported override meshes are often centered on their pivot, so
// the placement (pivot at terrain elevation) puts terrain through the MIDPOINT.
// ...
if (s_ground && autoGround) {
    const float dy = (s_ground==2) ? -out->minBox.y : -out->maxBox.y;
    // ... translates every vertex in Y by dy ...
}
```

- **Static props** (`autoGround=false`): **no correction happens.** Whatever Y position your glTF's geometry occupies after axis-mapping is exactly where it renders. If your source file's pivot doesn't already match the stock prop's ground-contact point, the prop floats or sinks into the terrain — you must get this right yourself (see [`../building-import/README.md`](../building-import/README.md) for a worked box example — note it's subject to the same mod-overlay bug described above, since both classes share the one registry).
- **Trees** (`autoGround=true`): the engine automatically re-translates the whole mesh so one end of its bounding box lands on MC2 `Y=0` — tunable via `MC2_GLTF_GROUND` (default `2`; `0` disables the correction entirely and behaves like a static prop). In practice this means a tree override forgives an imprecise pivot; a static-prop override does not.

Both classes share the same axis convention (`MC2_GLTF_AXIS=0` default: `mc2.x=-gltf.x`, `mc2.y=-gltf.y`, `mc2.z=gltf.z`, `mclib/assimp_importer.cpp:60-70`) — author your source glTF with Y in `[-HEIGHT, 0]` (canopy at the bottom in glTF-author-space) so it lands right-side-up at MC2 `Y` in `[0, HEIGHT]`.

## 3. Stock data reference

Verified against the deployed install at `A:/Games/mc2-opengl/mc2-win64-v0.4/data/tgl/`:

| File | Content |
|---|---|
| `data/tgl/oak1.ini` | `[TGLData] st FileName0="oak1"` — confirms the override key is `tree:oak1` |
| `data/tgl/128/a_oak1.tga` (+ `.ktx2`) | Stock oak1 leaf texture, 128×128 RGBA (has an alpha channel — confirmed via PIL) |

`oak1` is also already used as a worked example in this checkout's own `data/model_overrides/models.json` (real repo content, not this example's payload) alongside `tc1_1..4`, `palm1`, `palms` — all real stock tree appearance names, cross-referenced in `docs/superpowers/specs/2026-06-04-tree-lod-bake-pipeline-design.md:215`.

If you don't have a deployed build handy, `docs/tgl/<name>.ini`'s `FileName0` value is always your `<appearanceName>`; a sibling `mc2-srcdata` checkout (if you have one) is another place to look.

## 4. Install and verify

### Step 1 — get your own copy of the game

You need a working `mc2.exe` install you control (built yourself, or a distributed copy). **Do not** point this at this repository's own dev deploy roots (`A:/Games/mc2-opengl/mc2-win64-v0.4` or `-0.4c`) — `mc2mod.py install` refuses those on purpose (see `../getting-started.md` §5).

### Step 2 — get the mod payload

This example's finished mod folder already exists in this repo at:

```
docs/modding/examples/tree-import/mod/
```

Copy it (or clone this repo and reference the path) — you don't need to run the generator script unless you want to modify the placeholder mesh.

### Step 3 — install it

Pick ONE of these two paths.

**Option A — manual copy (fastest for iterating):**

```powershell
cd <path-to-your-mc2-modding-docs-checkout>
Copy-Item -Recurse -Force "docs\modding\examples\tree-import\mod" "<your-game-folder>\mods\tree-import-example"
```

**Option B — `mc2mod.py` pack + install (matches how you'd ship it to someone else):**

```powershell
py -3 tools\mc2mod\mc2mod.py pack docs\modding\examples\tree-import\mod --out .
py -3 tools\mc2mod\mc2mod.py verify-lite tree-import-example-0.0.0.mc2mod
py -3 tools\mc2mod\mc2mod.py install tree-import-example-0.0.0.mc2mod --deploy "<your-game-folder>"
```

Either way you end up with `<your-game-folder>\mods\tree-import-example\data\model_overrides\models.json` on disk — that's a fully valid, correctly-shaped mod install. It just isn't enough by itself to make the tree render differently yet: see Step 3b.

### Step 3b — REQUIRED workaround: activate it at the base-game path

Because of the engine bug documented in the AVAILABILITY box, `mods\tree-import-example\data\model_overrides\models.json` is never read. The only path the engine actually consults is `<your-game-folder>\data\model_overrides\models.json` (and the `.glb` under `<your-game-folder>\data\model_overrides\cooked\...`), regardless of `MC2_ACTIVE_MOD`. Copy the payload there directly:

```powershell
$overridesDir = "<your-game-folder>\data\model_overrides"
New-Item -ItemType Directory -Force $overridesDir | Out-Null   # some installs won't have this folder yet at all
if (-not (Test-Path "$overridesDir\models.json")) {
    # No base file yet (the stock 0.4 deploy in this repo ships none) — plain copy is safe.
    Copy-Item -Recurse -Force "docs\modding\examples\tree-import\mod\data\model_overrides\*" $overridesDir
} else {
    # A models.json already exists (this repo's own dev deploy target does, with real
    # oak1/tc1_1..4/palm1/palms entries — see §3). DO NOT overwrite it wholesale, or you'll
    # delete those entries. Instead copy the cooked asset folder, then hand-merge the one
    # override object below into the existing file's "overrides" array.
    Copy-Item -Recurse -Force "docs\modding\examples\tree-import\mod\data\model_overrides\cooked\oak1_leafcard" "$overridesDir\cooked\oak1_leafcard"
    Write-Host "Now open $overridesDir\models.json and add this object to its 'overrides' array:"
    Get-Content "docs\modding\examples\tree-import\mod\data\model_overrides\models.json"
}
```

This is the only way to observe the mechanism working on this checkout today. The `mods\tree-import-example\...` copy from Step 3 stays in place too — it's harmless, and it's what will start working automatically once the overlay bug is fixed (no doc change needed on your end at that point).

### Step 4 — launch

```powershell
cd <your-game-folder>
$env:MC2_MODOVERRIDE_TRACE = "1"
.\mc2.exe
```

`MC2_ACTIVE_MOD` is optional here — leave it unset, or set it to `tree-import-example` if you also want to exercise the (currently inert, for this mechanism) mod-overlay indexing alongside it. Either way behaves identically for this example.

### What you should see

- On stdout/stderr near startup: `[MODOVERRIDE] registry loaded: N override(s)` where `N` is at least `1` — exactly `1` if you did the clean-copy branch of Step 3b, or `1` higher than whatever your base `models.json` already had if you did the merge branch.
- Loading any mission that places `oak1` trees (mission-dependent — oak1 is a common temperate-map tree), watch for `[MODOVERRIDE_TRACE] tree 'oak1'` followed by `[MODOVERRIDE] tree 'oak1': render override applied (<path>)`.
- In-game: `oak1` trees render as a simple leaf-card cross (two crossed textured quads) using the stock leaf texture, instead of the full stock tree mesh. Collision/footprint is unchanged — you can still walk/drive through the same space you could before (or not — whatever oak1's stock collision already does).
- If you see `[MODOVERRIDE] tree 'oak1': import failed (...), using stock render` instead, the override was found but the `.glb` didn't import — check the path under `data/model_overrides/cooked/oak1_leafcard/` survived the copy/install intact, and re-run with `MC2_ASSIMP_TRACE=1` for detail.

## 5. How the payload was built

`mod/source/make_placeholder_glb.py` is a pure-stdlib script that hand-emits the `.glb` binary (no Blender/Assimp/external tooling needed) — the same technique used by the `building-import` example. It authors two crossed alpha-tested quads, front+back-faced, in the axis convention above, with a material named `a_oak1` whose base-color image is named `a_oak1.png`. The engine's `DeriveMC2TextureName` (`mclib/assimp_importer.cpp:173-`) derives the MC2 texture name from that image's file stem (`a_oak1` → `a_oak1.tga`) and binds the **stock** `data/tgl/128/a_oak1.tga` already in your install — no new texture asset is cooked.

```powershell
cd docs\modding\examples\tree-import\mod\source
py -3 make_placeholder_glb.py oak1_leafcard.glb
```

Output actually produced by this run: `16 verts, 8 tris, glTF bounds [-5.0, -14.0, -5.0]..[5.0, 0.0, 5.0]` (MC2 Y ends up `0..14.0` after the axis map, before the tree's `autoGround` correction runs at import time).

`mod/data/model_overrides/cooked/oak1_leafcard/manifest.json` is a full `mc2-asset-manifest-v1` cook-manifest (the same format `tools/asset_cook/trackg_cook.py` would emit for a real cooked asset) documenting this placeholder's geometry/material/provenance — hand-written here since this example doesn't run the real cook pipeline, but shaped to validate against the same schema. `materials.json`/`staged.json`/`models.generated.json` alongside it are the intermediate authoring artifacts a real cook run produces (mirrors the `building-import` example's layout).

## 6. Validator run (actually executed against this payload)

```
$ py -3 tools/asset_cook/validate_asset_manifest.py docs/modding/examples/tree-import/mod/data/model_overrides/cooked/oak1_leafcard/manifest.json
PASS manifest.json format=cook id=oak1_leafcard replaces=tree:oak1 materials=1
(exit 0)
```

```
$ py -3 tools/mc2mod/mc2mod.py pack docs/modding/examples/tree-import/mod --out <scratch-dir>
pack: <scratch-dir>\tree-import-example-0.0.0.mc2mod
  id=tree-import-example  version=0.0.0  files=9  sha256=34432f7108a5d1fee2a3726f155659daf09aa025714c13c0d00770be2bea6bd7

$ py -3 tools/mc2mod/mc2mod.py verify-lite <scratch-dir>\tree-import-example-0.0.0.mc2mod
verify-lite PASS: id=tree-import-example  version=0.0.0  files=9  pkg_sha256=34432f7108a5d1fee2a3726f155659daf09aa025714c13c0d00770be2bea6bd7
(exit 0)

$ py -3 tools/mc2mod/mc2mod.py install <scratch-dir>\tree-import-example-0.0.0.mc2mod --deploy <scratch-deploy-dir>
install: tree-import-example v0.0.0 -> <scratch-deploy-dir>\mods\tree-import-example
  files installed : 9
```

The install confirmed the payload lands correctly at `mods/tree-import-example/data/model_overrides/models.json` — that's the standard, forward-compatible overlay drop path for a moddable asset. It's just not (yet) the path this *particular* asset type is actually read from at runtime — see the AVAILABILITY box and Step 3b.

`tools/mod_install/resolver.py` (mentioned in `../mechanism-map.md`) is a library module with no CLI entry point in this checkout — it's consumed by other tooling (e.g. parity-smoke scripts), not directly runnable against a single payload, so it isn't part of this validation run.

## 7. Troubleshooting

| Symptom | Try |
|---|---|
| No `[MODOVERRIDE]` lines at all | This line prints unconditionally, once, from `ModelOverrideRegistry::instance()` at startup — `MC2_ACTIVE_MOD` does NOT gate it. If it's truly missing, you're not looking at stderr (it's `fprintf(stderr, ...)`, not stdout), or the exe predates this feature. This is unrelated to whether your mod is active. |
| `[MODOVERRIDE] registry loaded: 0 override(s)` | Expected and harmless if you skipped Step 3b: **your mod's `data/model_overrides/models.json` is never read regardless of `MC2_ACTIVE_MOD`** — this is the confirmed bug in the AVAILABILITY box, not a mod-indexing problem. Do NOT chase this with `MC2_LOG_FILE_RESOLVE`/`MC2_REBUILD_MOD_CACHE` — those only diagnose the general file overlay, which is working fine and is not the bottleneck here. Go do Step 3b: confirm `<your-game-folder>\data\model_overrides\models.json` exists on disk with your `oak1` entry in its `overrides` array. |
| Registry loads (`N override(s)`, `N>=1`) but no per-tree trace lines | The mission you loaded doesn't place any `oak1` instances — try a different mission, or repoint `replaces` at a tree type you know the mission uses |
| `import failed` in the log | Re-run with `MC2_ASSIMP_TRACE=1`; also confirm the `.glb` at `<your-game-folder>\data\model_overrides\cooked\oak1_leafcard\oak1_leafcard.glb` (the base-game path from Step 3b, not the `mods\...` copy) actually exists and wasn't dropped by the copy step |
| Tree renders but looks vertically wrong (buried/floating) | Try `MC2_GLTF_GROUND=0` (disables auto-ground — should look wrong differently) vs the default `2`, and `MC2_GLTF_YOFF=<float>` to nudge; trees tolerate this better than static props (see §2) but a wildly wrong source mesh can still confuse it |
| Shadow looks like a solid rectangle, not a leaf silhouette | Known, unrelated to this example: `SHADOW-FOLIAGE-ALPHA-DISCARD-1` (`docs/known_issues.md:41`) — the depth-only shadow pass doesn't apply the alpha-test cutout for any tree card, stock or override, deferred |

## 8. Known limitations of this example

- **The mod-overlay doesn't apply to this mechanism (engine bug)** — see the AVAILABILITY box and Step 3b. Until `mclib/model_override_registry.cpp:39-45,158-166` (manifest) and `mclib/assimp_importer.cpp:475-486` (geometry) are routed through the engine's mod-aware file resolver, a mod's `data/model_overrides/...` files must be copied to the base-game path to take effect. This is the single biggest gotcha in this example — read it before assuming Steps 1-4 alone will work.
- **LOD is out of scope** — see the AVAILABILITY box and `docs/HANDOFF-model-override-lod.md`. Don't add a `lods` array to `models.json` for a tree entry; it parses, but the light-slot bug means it's not verified safe.
- **The placeholder mesh is intentionally crude** (two crossed quads) — this doc is about the override *mechanism*, not about producing good tree art. Swap `mod/source/oak1_leafcard.glb` for your own authored asset (same axis/material-naming convention) to get something that actually looks like a tree.
- **One texture, reused from stock** — no new `.tga`/`.ktx2` was cooked; this keeps the example self-contained. A real mod would typically ship its own leaf texture under `data/tgl/128/` and reference it by name the same way.
