# CEMENT-OUTLINE-4 — STATUS

Branch `claude/nifty-mendeleev`. Ship commit **`62f789c2`** (CONCRETE-DETAIL-NORMAL-1).
Lane pivoted mid-flight by a user ruling; both halves recorded below.

## 1. Why the v0.3 install looks different — ANSWERED, with receipts

The user asked whether other installs could be "rolled back" to v0.3. Establishing
*what actually differs* first:

| Hypothesis | Verdict | Evidence |
|---|---|---|
| v0.3 is an older exe predating the detail work | **FALSE** | `deploy_status("0.3")`: embedded sha `c68896963b20` == HEAD at lane start; manifest `src_commit` identical, `dirty=false`, `stale=false` |
| v0.3 has different material tuning data | **FALSE** | `concrete_normalBoost: 2.5` in BOTH `mc2-win64-v0.3/data/terrain_materials.json` and the 0.5-testing install's |
| v0.3 ships different terrain assets | **TRUE — this is it** | `mc2-win64-v0.3` has **no `data/terrain_layers/` directory at all**, so `matNormalArray` never loads and the concrete relief never appears |

**Per-install asset census** (this is the real inconsistency):

| Install | `terrain_layers/` |
|---|---|
| `mc2-win64-v0.3` (user's reference) | **absent** |
| `mc2-win64-v0.4`, `v0.4c`, `abl-validate` | 6 files — **albedo only, no `*_normal.ktx2`** |
| `mc2-win64-v0.4d-rc1`, `releases/0.5.1` | **absent** |
| `releases/0.5 testing/mc2-win64-v0.5.0` | 16 files — albedo **+ normals** |

So the disliked concrete detail *normal* could only ever have been active in the
**0.5-testing** install. Everywhere else it was already inactive. Worth flagging to
the user: if they are seeing objectionable concrete in an install other than
0.5-testing, the detail normal is **not** the cause and the ruling has not fixed it.

## 2. What shipped — `MC2_TERRAIN_CONCRETE_DETAIL_NORMAL`, default OFF

The concrete detail-normal branch (`terrain_lod_chunk.frag:1402-1405`,
`MAT_LAYER_PAINTED_CONC`) was **ungated** — it ran whenever `pureConcrete > 0.001`:

```glsl
dN += pureConcrete * matNormalBoost.w * fwC * (cN.rgb * 2.0 - 1.0);
```

`matNormalBoost.w` (`nb[3]`, JSON `concrete_normalBoost = 2.5`) is its **only** scale,
so zeroing `nb[3]` at the uniform upload in `gos_terrain_lod_chunk.cpp` makes the
branch contribute exactly `vec3(0)`. **Default OFF = original overlay look.** `=1`
restores the relief.

- **Shader untouched** — no recompile, no risk to other passes.
- Rock/grass/dirt detail normals (`nb[0..2]`) untouched; cement albedo/atlas path untouched.
- Registered (`RendererFeatureRegistry.h`), documented (`docs/tier1_env_vars.md`),
  smoke-allowlisted (`run_smoke.py`).

## 3. Measurement — floor MEASURED, not recorded

Test bed `mc2-win64-abl-validate` (exe `62f789c2`; normal layers copied in from
0.5-testing so the "before" state actually exists — the install ships albedo only).

| Run | pose `mc2_05_saved_101748` | hash |
|---|---|---|
| Noise floor, 2 runs, default | byte-identical | `cab56f6f3553` ×2 → **0.000%** |
| Relief ON (`=1`), 2 runs | byte-identical | `69ea6fab491c` ×2 |

Gate is live (hash changes, ≫ floor). Delta at that pose: **1.05% of pixels, max 7/255,
mean 3.49**. All PNGs 1 440 773 B (non-zero — disk was at 12 GB).

**Honest read:** the effect at the user's mc2_05 water/shoreline bookmark is *small*.
That pose is at altitude with little concrete in frame and the anti-tile LOD fade
already attenuates detail with distance. Do not over-sell this as the cure for the
mc2_23 outlines.

## 4. The mc2_23 pad outlines — mechanism identified, NOT yet capture-confirmed

Separate from the ruling. Source analysis (`terrain_lod_chunk.frag`) found the
albedo and lighting blends run on **different curves** across a cement boundary:

- albedo: `concreteColorBlend = sqrt(pureConcrete)` → line 1535
- lighting: `normalLight = mix(normalLight, 1.0, pureConcrete)` → line 1786 (linear)
- composite: `lit = baseColor * normalLight * shadow` → line 1866

`sqrt(t) > t`, so **cement albedo arrives before the lighting exemption does**. The
transition ring renders as cement-toned surface still carrying terrain lighting.
`normalLight = mix(0.35, 1.20, diffuse)`, so at **night** the exemption is a ~3×
jump and the mismatch is maximal; by day (`≈1.2` vs `1.0`) it nearly vanishes —
consistent with the complaint arriving from a **night** mc2_23 capture.

Also found: `pavedOverlayFlat` (line 1794) sets `normalLight = 1.0` as a **hard,
per-tile, unfeathered** flag. Prediction: `MC2_PAVED_OVERLAY_CLASS=1` would **add**
hard-edged outlines rather than remove them. **This predicts the standing
ROAD-OVERLAY-LIGHT-1 attribution will fail its own test.** Not verified — the lane
pivoted before the ON/OFF capture matrix ran. Do not report as closed.

Candidate one-line fix (untested): drive the lighting exemption with
`concreteColorBlend` instead of `pureConcrete` so both arrive together. Preserves
the user-approved crisp CEMENT-HARD-EDGE-1 alpha shape (only the curve changes).

## 5. Rollback question — recommendation

**Do not roll the installs back to the v0.3 binary.** The difference was never the
exe (identical sha) — it was *which assets got deployed*. Freezing stale binaries
would discard every unrelated fix since, and would not be reproducible for future
builds. The default flip in `62f789c2` is the correct form: future builds are right
by construction, and installs get re-deployed from it.

## 6. Not done / open

- tier1 with explicit `--exe` + `DEPLOY_FINGERPRINT OK` — **not run**.
- mc2_23 overview, `mc2_05_saved_102009`, mc2_01, day/night ×6-map matrix, class-diag
  overlays — **not captured**.
- `MC2_PAVED_OVERLAY_CLASS` ON/OFF at the user's poses — **not run** (§4 prediction
  stands untested).
- `abl-validate` has `*_normal.ktx2` copied in by this lane; revert if that install's
  shipped content matters.
