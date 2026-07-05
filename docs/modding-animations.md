# Modding Mech Animations (Drop-In)

How to **replace a mech's animation clips** (walk, run, idle, hit reactions, …) without
recompiling the engine. This works **today** through the mod-overlay file system — no engine
change required. Companion to [gosfx-modder-dropin-path.md](gosfx-modder-dropin-path.md) (which
covers swapping weapon/explosion **FX**).

> Status: animation **drop-in / replacement** works now. Authoring new clips from Blender/GLTF
> does **not** yet (no `.ase`/`.agl` exporter exists — see [Gaps](#gaps)). For now you replace
> with `.ase`/`.agl` files you already have (e.g. from another mech, or hand-edited).

---

## How MC2 animations work (the 30-second model)

- MC2 mechs are **TGL** meshes (`mclib/msl.*`, `mclib/tgl.*`), animated by class `TG_AnimateShape`.
- An animation is **per-node TRS keyframes** — for each mesh node, a list of
  `quaternion[frame]` (rotation) + `point3D[frame]` (position), plus a frame rate. It is **not**
  a skinned skeleton and **not** per-vertex morph. (`_TG_Animation`, `mclib/msl.cpp:2458-2516`.)
- Source format is 3DS-MAX **`.ase`** (ASCII Scene Export). The engine **cooks** it to a binary
  **`.agl`** on first load and reuses that.

## The cook (why editing an `.ase` "just works")

`TG_AnimateShape::LoadTGMultiShapeAnimationFromASE` (`mclib/msl.cpp:2608`):

1. If a matching `.agl` exists **and** its version == `CURRENT_ANIM_VERSION` (`0xBADDECAF`),
   load the binary `.agl` directly (fast path).
2. Otherwise parse the `.ase` and **write a fresh `.agl`** next to it.

The check is **mtime + version gated**, so if you edit/replace the `.ase`, the stale `.agl` is
re-cooked automatically. (If you ship a pre-cooked `.agl`, it's used as-is — fastest, but must be
the current version.)

## Naming convention = the implicit manifest

Each mech loads its clips by **filename convention** (`mclib/mech3d.cpp:550-578`):

```
<tglPath>/<mechName><GestureSuffix>.ase     (or .agl)
```

`<mechName>` is the mech's TGL base name (e.g. `madcat`). `<GestureSuffix>` comes from
`MechAnimationNames[]` (`mclib/mech3d.cpp:265-294`). A missing file is **not an error** — the mech
just freezes on that gesture and moves on.

| Gesture | Suffix | Example file |
|---|---|---|
| Stand (idle pose) | *(empty)* | `madcat.ase` |
| Park / power-down | `StandToPark` | `madcatStandToPark.ase` |
| Park → stand | `ParkToStand` | `madcatParkToStand.ase` |
| Stand → walk | `STtoWK` | `madcatSTtoWK.ase` |
| **Walk** | `Walk` | `madcatWalk.ase` |
| Walk → run | `WKtoRN` | `madcatWKtoRN.ase` |
| **Run** | `Run` | `madcatRun.ase` |
| Run → walk | `RNToWK` | `madcatRNToWK.ase` |
| Stand → reverse | `WKtoST` | `madcatWKtoST.ase` |
| Limp left / right | `LimpLeft` / `LimpRight` | `madcatLimpLeft.ase` |
| **Idle** (fidget) | `Idle` | `madcatIdle.ase` |
| Fall backward / forward | `FallBackward` / `FallForward` | `madcatFallForward.ase` |
| Hit front/back/left/right | `HitFront`/`HitBack`/`HitLeft`/`HitRight` | `madcatHitFront.ase` |
| **Jump** | `Jump` | `madcatJump.ase` |
| Get up (from back / front) | `GetupBack` / `GetupFront` | `madcatGetupBack.ase` |
| Destroyed (back / forward) | `FallBackwardDam` / `FallForwardDam` | `madcatFallForwardDam.ase` |

(Gesture IDs in `mclib/mech3d.h:64-88`. Foot-down frames in the clip drive pathing, so keep the
contact frames sensible.)

---

## Replace an animation (step by step)

1. Make / obtain the replacement clip as `<mechName><Suffix>.ase` (or a current-version `.agl`).
2. Put it in your mod under the TGL data path:
   ```
   mods/<yourMod>/mod.json
   mods/<yourMod>/data/tgl/madcatWalk.ase        # the new walk clip
   ```
   `mod.json` (minimal):
   ```json
   { "schema": "mc2-mod/1", "id": "yourMod", "name": "Your Mod", "version": "1.0.0", "dependencies": [] }
   ```
3. Launch with your mod active:
   ```
   MC2_ACTIVE_MOD=yourMod
   ```
   The overlay (`mclib/file.cpp` `TryModOpen`, precedence **base < dependency < active**) resolves
   `data/tgl/madcatWalk.ase` from your mod first. The engine re-cooks it to `.agl` on load.

That's it — no rebuild. To revert, remove the file or unset `MC2_ACTIVE_MOD`.

### Declarative remap (anims.json) — rename-free swap

If you'd rather not match the stock filename, declare the remap in a manifest and
point a gesture at any clip name. Put `anims.json` in your mod:
```
mods/<yourMod>/data/anim_overrides/anims.json
mods/<yourMod>/data/anim_overrides/my_strut.ase     # your clip (cooks to .agl here)
```
```json
{ "overrides": [
  { "type": "anim", "replaces": "mech:madcat", "gesture": "Walk",
    "source": "my_strut.ase", "fallback": "stock" }
] }
```
- `replaces` = `mech:<mechName>`; `gesture` = a suffix from the table above (case-insensitive).
- `source` = a relative `.ase`/`.agl` in the manifest dir (no absolute paths / `..`).
- A matched (mech,gesture) loads your clip; everything else stays stock. Invalid entries are
  logged `[ANIMOVERRIDE] dropped ...` and ignored (never fatal). Launch with `MC2_ACTIVE_MOD`.
- This is additive over a base `data/anim_overrides/anims.json`; mod entries win on duplicate key.

### Tips
- Ship the **`.ase`** (human-diffable, auto-recooks) unless you specifically pre-cooked a
  current-version `.agl`. A stale-version `.agl` is ignored and re-derived from the `.ase`.
- You can copy another mech's clip — animations are keyed by node, so retargeting only works if
  the node names line up (same chassis family). Mismatched nodes are skipped (partial freeze).

---

## Swapping FX too (pointer)

Weapon/explosion FX swap through a parallel path — a per-effect JSON sidecar
(`data/effects/defs/<EffectName>.fxdef.json`, `MC2_FX_DEFS=1`) for retuning/replacing an
**existing** effect's texture/blend/color/scale, no `mc2.fx` rebuild required. See
[modding-effects.md](modding-effects.md) (field reference, `fxlint`, worked example) for that
path, or [gosfx-modder-dropin-path.md](gosfx-modder-dropin-path.md) for the whole-blob overlay +
`tools/mc2fx/` CLI (`dump`/`build`/`clone`/`new`) needed to add a **brand-new** effect name.
Bind a weapon to a different effect via `compbas.csv` "Special FX ID" → `effects.csv` names, or
a `<weapon>.fit` `HitEffect=`/`MissEffect=`.
Note: gosFX particles are **default-disabled in the current beta** (`MC2_DISABLE_GOSFX=1`).

---

## Gaps (what doesn't work yet)

- **No `.ase`/`.agl` exporter.** You can *replace* clips with existing files but can't easily
  *author* new ones from Blender/Maya/GLTF — there's no tool to bake arbitrary animation into the
  per-node `.agl` format keyed to TGL node names. This is the biggest authoring gap.
- **GLTF import ignores animation.** The modern `[Import] Source=<glb>` mesh path
  (`mclib/mech3d.cpp:367-449` → `assimp_importer.cpp:483`) loads geometry only; it logs but does
  **not** read `scene->mNumAnimations`. GLB swaps the *mesh*, not the *animation*.
- ~~No declarative override registry~~ — **shipped**: see
  [Declarative remap (anims.json)](#declarative-remap-animsjson--rename-free-swap) above. Still
  loads existing `.ase`/`.agl` files only (no authoring/retarget tool).
