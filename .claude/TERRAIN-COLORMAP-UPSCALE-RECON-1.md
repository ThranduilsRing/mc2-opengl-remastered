# TERRAIN-COLORMAP-UPSCALE-RECON-1 — Recon

**Branch:** claude/controlmap-sample-1  **Worktree:** `A:/Games/mc2-controlmap-sample-1`
**Scope:** RECON ONLY. No source changes, no build, no launch.

---

## 0. Headline finding — this was already done

**Offline AI upscale of stock-mission colormaps is not a future idea; it shipped.**
`docs/asset-pipeline.md` (verified 2026-06-17) row 1: colormap src `1280²` →
**4× ESRGAN → 5120²** atlas, archived at `release_assets/0.3/mc2-burnins-4x-pt1/2.zip`
(2.3 GB, not present in this worktree — external/LFS asset, doc-attested only, could
not byte-verify the zip itself). Deployed `.burnin.ktx2` BC7 files exist on disk for
all 5 tier1 missions at `A:/Games/mc2-opengl/mc2-win64-v0.3/data/textures/{mc2_01,03,10,17,24}.burnin.ktx2`.
Pipeline: `docs/terrain-colormap-modernization-debt.md` steps 1-4 (all shipped,
merged to nifty) — BC7 KTX2 atlas active by default (`MC2_COLORMAP_KTX2`), VRAM
108 MB → 27 MB (compressed) per full 5120² atlas.

## 1. Resolution / atlas / VRAM

- Source measure: `tools/terrain_gen/terrain_recipe.py:93 burnin_resolution()` =
  `(map_size_verts // 20) * 256` (engine-matched, pre-upscale). `burnin_renderer.py:37`
  caps final output at 4096² by design ("detail comes from normal/detail maps, not
  huge baked colormaps") — working_cap defaults to `min(2048, target)`, i.e. the
  *procedural generator* shades at ≤2048² even when the shipped stock colormaps are
  higher-res, because stock missions are hand/legacy-baked, not generator output.
- Stock/shipped: 1280² native → 5120² after 4× ESRGAN (`4x-UltraSharpV2.safetensors`,
  per `docs/modding-guide.md:206-214`, via `realesrgan-ncnn-vulkan`).
- Loader has **no packing-grid ceiling** — `BuildColormapAtlas` (`gos_terrain_indirect.cpp:997`)
  uploads a single `glCompressedTexImage2D` texture (not a tiled/packed atlas despite
  the name); only constraint enforced is `width%256==0` (`numTexturesAcross` UV math,
  `check-texture-resolution.py:10-12`) and driver `GL_MAX_TEXTURE_SIZE` (checked at
  `gos_render.cpp:413`, typically 16384 on any GL4.3-capable GPU — no headroom problem).
  `check-texture-resolution.py:54` explicitly whitelists 5120² as non-"bomb" for Pillow.
- VRAM math (BC7 = 1 byte/texel, mip0 only — `--levels 1`, no chain):
  - current 5120² BC7: **25 MB/mission** (RGBA8 fallback would be 100 MB).
  - a further 1.5× (→ ~7680²): **56 MB/mission** (+31 MB).
  - a further 2× (→ 10240², i.e. 8× original): **100 MB/mission** (+75 MB).
  - ×5 tier1 missions resident simultaneously is not the model — only the active
    mission's atlas is resident, so per-mission delta is what matters, not ×5.

## 2. Existing tooling verdict

**Real, working, in-repo — not just noted.** Concretely:
- `docs/modding-guide.md:206-214` — setup: `realesrgan-ncnn-vulkan` binary +
  `4x-UltraSharpV2.safetensors` model in `esrgan_models/`, driven by `upscale_gpu.py
  --input data/art/ --output mc2srcdata/art_4x_gpu/ --scale 4` (this exact script
  targets `data/art/`, i.e. general art/TGL — colormaps used the same family of
  tooling per the asset-pipeline doc's "4× ESRGAN" tag but the burnin-specific
  invocation/script is not itself checked into this worktree, only its output archive).
- `tools/mc2texcook/batch_upscale_cook.py` — the **cook half** already assumes
  externally-upscaled TGAs as input ("Does NOT perform any upscaling itself... For
  upscaling, use a dedicated tool first") and batch-cooks to BC7 KTX2. This is the
  reusable batch driver the recon asked about — it's generic (not burnin-specific)
  but has an `albedo` preset that matches colormap color-space needs (SRGB source →
  the actual `bake_colormap_ktx2.py` uses UNORM/linear instead, see §3 note).
- `tools/mc2texcook/cook_tgl_upscale_single.py` — Pillow LANCZOS fallback ("used
  when no AI upscaler... is available") for single-file cook+upscale, non-AI.
- CHANGELOG.md:47 confirms realesrgan-ncnn-vulkan is the established house tool for
  4× "art and TGL textures" — colormaps are one more consumer of the same lineage.
- **Verdict: tooling exists and works; the specific recon ask (2-4× AI upscale →
  re-bake → drop-in higher-res base color for stock missions) is a shipped, proven
  pattern, not a gap.** What's *not* yet built is a burnin-specific batch driver that
  chains extract→ESRGAN→`bake_colormap_ktx2.py` in one script; today that would be a
  manual `upscale_gpu.py`-style run per mission then `bake_colormap_ktx2.py`.

## 3. Pipeline design (if going further, e.g. a 2nd upscale pass)

- Extract: `.burnin.tga` already on disk per mission (`data/textures/<mission>.burnin.tga`,
  pre-KTX2 source, BGRA-native) — no extraction step needed, source is already flat TGA.
- Upscale: reuse `realesrgan-ncnn-vulkan` + `4x-UltraSharpV2` (proven house model per
  §2) at 1.5-2× (not another full 4×, current is already 4× off native 1280² — a 2nd
  4× pass would compound to 16× = 20480², absurd/no-benefit past display resolution).
  Model choice for terrain macro-color imagery: UltraSharpV2 already validated in
  this repo > generic x4plus (no evidence x4plus was tried/rejected, but UltraSharpV2
  is the established, working choice — don't re-litigate model choice without cause).
- Bake: `scripts/bake_colormap_ktx2.py` already does upscaled-TGA → BC7 KTX2 (accepts
  `--max-edge` for *downscale* cap only; upscale must happen before this script runs).
  **Color-space note:** `bake_colormap_ktx2.py` uses `--format R8G8B8A8_UNORM --assign-tf
  linear` (matches existing RGBA8 upload semantics) — NOT the `albedo`/SRGB preset in
  `batch_upscale_cook.py`. Any new burnin-specific batch driver must match the UNORM/
  linear convention, not naively reuse the `albedo` preset.
- Batch driver for tier1: no dedicated one-shot script exists yet; would be a thin new
  wrapper (or an argument added to `batch_upscale_cook.py`) that globs
  `data/textures/{mc2_01,mc2_03,mc2_10,mc2_17,mc2_24}.burnin.tga`, invokes ESRGAN per
  file, then calls `bake_colormap_ktx2.py --mission <stem>`. Not built; small (~1 file).
- Determinism/reproducibility: **not currently pinned.** No recorded ESRGAN model
  hash/version, no seed (ESRGAN inference is deterministic per-model but model file
  itself is user-supplied, unversioned in-repo — `esrgan_models/` is local-only per
  modding-guide, not committed). Re-running today's pipeline against the same model
  file reproduces bit-identical output; but nothing pins *which* model file was used
  for the shipped 4× pass, so a "redo it" run cannot be proven identical to the
  original 2.3 GB archive without that provenance. Recommend: if going further, record
  model filename+sha256 in a sidecar/manifest next to the bake (parallels the existing
  `check-texture-resolution.py` cook.json provenance pattern in §1 of asset-pipeline.md).

## 4. Interaction risks

- **Control-map albedo lift is NOT a garnish** — per `.claude/TERRAIN-CONTROLMAP-ALBEDO-1-RECON.md`,
  the burned-in colormap (`base`) retains **50-82% of final baseColor** always
  (`terrain_lod_chunk.frag:733-735`, `tintStrength` capped 0.18-0.50, only 0.85 under
  snow) — so colormap upscale quality has *outsized, not marginal* visual impact on
  final pixels; this is a real payoff, not cosmetic-only.
- **Cement burn-in / colorkey edges**: cement pads are a *separate* atlas/axis
  (`gos_terrain_indirect.cpp:4140` cement atlas, `terrain_lod_chunk.frag:544-570`
  overwrites `base` where `cementWordF` valid) — colormap upscale does not touch
  cement pads directly. Risk is at the **boundary**: AI upscalers (ESRGAN family)
  hallucinate/soften sharp edges; if any colorkey-style hard edge exists in the raw
  1280² source (e.g. hard cement/terrain boundary baked into the colormap itself
  rather than the separate cement atlas), a 4× ESRGAN pass could blur or ring that
  edge. Not verified against actual mission source in this recon (would need visual
  side-by-side, see §5) — flag as the one plausible regression vector.
- **BC7 block artifacts on upscaled gradients**: BC7 is already in use post-upscale
  today (shipped 5120² is BC7) with `--levels 1` (no mip chain) — any further upscale
  inherits the same BC7 4×4 block quantization on smooth AI-generated gradients
  (ESRGAN output tends to be smoother than native photo/paint sources, so this is
  probably *lower* risk than typical BC7-on-noisy-photo artifacting, not higher).
- **GPU displacement**: `mapdata.cpp cpu_sampleColormap` (CPU displacement) was
  already retired (`MC2_COLORMAP_CPU_RETIRE` default ON, per debt doc step 2) — no
  CPU-side resolution-dependent sampling left to worry about; purely a GPU texture
  swap.

## 5. Acceptance / go-no-go

- Loader takes any resolution divisible by 256 (§1) — **no engine change needed**
  confirmed; this is pure data (bigger `.burnin.tga` → `bake_colormap_ktx2.py` → same
  `.ktx2` filename → existing sidecar-probe loader picks it up automatically).
- No atlas packing ceiling found; `GL_MAX_TEXTURE_SIZE` not a practical constraint
  until ~16384² (would need a further ~13× beyond the current 5120² shipped state).
- Acceptance would be: workbench side-by-side crops (native 1280² vs current shipped
  5120² vs any further candidate) at close-camera cutscene framing + one tier1 smoke
  run per `CLAUDE.md` canonical command (regression-only; visual change has no
  code-path risk since it's data-only).

---

## Summary (≤10 lines)

**Current res:** stock colormap 1280² native → already 4× ESRGAN'd to 5120² BC7
(shipped, `release_assets/0.3/mc2-burnins-4x-*`, all 5 tier1 `.burnin.ktx2` present on
disk at mc2-win64-v0.3). Loader has no packing ceiling, just `width%256==0` + driver
`GL_MAX_TEXTURE_SIZE` (~16384, non-binding).
**Existing-tooling verdict:** real and proven — `realesrgan-ncnn-vulkan` +
`4x-UltraSharpV2.safetensors` (house model, `docs/modding-guide.md`), plus
`batch_upscale_cook.py`/`cook_tgl_upscale_single.py`/`bake_colormap_ktx2.py` as the
cook chain. The exact recon ask (offline AI-upscale → BC7 rebake for stock missions)
is not a gap, it already shipped.
**VRAM math:** 5120² BC7 = 25 MB/mission today; a further 1.5×(→7680²) = 56 MB
(+31 MB); a further 2×(→10240², 8×-orig) = 100 MB (+75 MB). Trivial on any GL4.3 GPU.
**Pipeline (if going further):** (1) reuse existing `.burnin.tga` source + house
ESRGAN model at 1.5-2× (not another full 4× — compounds absurdly), matching
`bake_colormap_ktx2.py`'s UNORM/linear convention (not the SRGB `albedo` preset in
batch_upscale_cook.py); (2) no burnin-specific batch driver exists yet — small
new wrapper needed chaining ESRGAN→`bake_colormap_ktx2.py --mission <stem>` per
tier1 mission.
**Risks:** colormap retains 50-82% of final albedo (NOT diluted by splat weights —
real payoff, real risk both ways); cement pads are a separate atlas so low direct
risk, but any hard colorkey edge baked into the raw colormap itself risks
ESRGAN blur (unverified, needs visual check); BC7 block artifacts on AI-smoothed
gradients likely mild since ESRGAN output is smoother than typical photo noise.
**Determinism gap:** ESRGAN model file is local-only/unversioned in-repo; no
sha256/manifest pins which model produced the shipped 5120² archive — a "redo it"
run cannot be proven bit-identical without recording that.
**Go/no-go:** the originally-envisioned work (2-4× AI upscale of stock colormaps)
is **already shipped and live** — no-go on redoing it. A **further, smaller**
push (1.5-2× beyond today's 5120², i.e. targeting the still-muddy close-camera
case) is **conditional-go**: cheap (VRAM trivial, no engine change), but needs (a)
a visual side-by-side first to confirm ESRGAN doesn't already look soft at 5120²
(diminishing returns risk — a 2nd pass upscaling already-upscaled pixels adds no
real detail, only smoothing), and (b) the missing batch-driver wrapper (~1 file,
small).
