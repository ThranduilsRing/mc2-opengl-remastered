# TILEHIRES — next slices (for opus planning)

Status of OVERLAY-TILE-HIRES-1 (branch `claude/tilehires-1`, HEAD `08362c25`):
loader + cook + allowlist + docs SHIPPED, gate `MC2_OVERLAY_TILE_HIRES` default
OFF byte-identical. 469 tiles (17 road/runway/crossing/bridge families) cooked
64→256 colorkey-safe and staged in two installs:

- `A:/Games/mc2-opengl/mc2-win64-v0.3/data/textures/256Overlays` (spare lane, validated)
- `A:/Games/mc2-opengl/releases/0.5 testing/mc2-win64-v0.5.0/data/textures/256Overlays`

Crop sheet: `overlay_tile_hires_contact_sheet.png` at both install roots
(64-vs-256 side-by-side, 536x1120).

Tiles are NOT in git (release_assets convention) — coordinator merge wave must
carry the loose folder alongside the exe, or re-run the cook
(`tools/overlay_upscale/overlay_tile_upscale.py --deploy <lane> --out-dir ...`,
deterministic given the same model file `esrgan_models/4x-UltraSharpV2.safetensors`).

## 1. Remaining tile families

475 loose 64Overlays tiles total; 469 cooked. Deliberately excluded:

| Family | Tiles | Why excluded / next step |
|---|---|---|
| `cementpattern00..02` | 3 | Cement TRANSITION machinery — consumed by the cement bake/combine path, not plain overlay quads. Upscaling changes transition-bake input stride; needs its own parity check against `combineOverlayTxm()` nearest-resample branch and the CEMENT lane (CEMENT-BAKE-INTO-TERRAIN-RECON-1). Small win, do with cement owner. |
| `scorch*` | 3 | FX decals (burn marks). Loaded through the same initOverlay path but visually soft-edged; ESRGAN sharpening may look wrong. Candidate for a plain Lanczos cook or the decal-integrate lane instead. |

Everything else in 64Overlays is cooked. No 64Overlays entries exist only in
`textures.fst` for these families (loose set was complete at cook time).

## 2. 512 tier

Loader already accepts pow2 in [128,1024] (`MC2_OVERLAY_TILE_HIRES=512` →
probes `512Overlays/`, per-file fallback chain is 512 → 64 legacy, NOT
512 → 256 → 64 — fallback is single-hop to the legacy folder by design).

- Cook: `overlay_tile_upscale.py --edge 512` works today (model is 4x; 512
  needs 4x then 2x, or a second model pass — script currently does one 4x pass
  to 256 then Lanczos to `--edge`; verify quality before shipping).
- Memory: base-mip sysRAM copy is `edge^2 * 4` per tile held in `tileRAMHeap`
  for every overlay type the mission uses. 256 = 256 KB/tile; 512 = 1 MB/tile.
  A road-heavy mission loading ~50 tiles: ~13 MB (256) vs ~50 MB (512) sysRAM
  + same order VRAM. 256 is comfortable; 512 needs a look at `tileRAMHeap` size
  and lazy-load behavior before defaulting.
- MIPS: hires applies to the base level only (`j == 0` in `initMipTexture`);
  mip folders `32/16/8Overlays` stay legacy. At 512 the base/mip gap widens
  further — if distant shimmer shows up, next slice should generate hires mip
  folders (`256Overlays`+`128Overlays` as mips of 512) or let the GPU autogen
  mips instead of the folder scheme.

## 3. FST packing interaction

- Cook SOURCES prefer loose `data/textures/64Overlays` over `textures.fst`
  entries (zlib, classic-LZW fallback via `tools/mc2x_import/fst.py`).
- Cook OUTPUT is loose-only. The loader probe (`File probe; probe.open(...)`)
  goes through `File::open`, which consults fast files too — so a future
  `textures.fst` repack COULD carry `data\textures\256Overlays\*.tga` and the
  gate would find them with zero code change. Not exercised; if the release
  packer moves them into an FST, add one gate-ON smoke to prove the probe hits
  the archived path (case/hash of the longer path is the risk point).
- `128Overlays/` ships in stock data and was historically dead weight
  (TERRAIN_TXM_SIZE clamped to 64). `MC2_OVERLAY_TILE_HIRES=128` now makes it
  live — free low-cost tier, but stock 128 tiles are NOT the ESRGAN set;
  visual quality unverified.

## 4. A/B vs markings-v2 sidecar

The markings-v2 sidecar (controlmap-sample lane, `5be735a7`) covers marking
crispness via vector overlay; THIS lane's value is whole-tile fidelity
(asphalt/dirt texture detail included). A/B both on a runway mission
(mc2_01 has runway + roads) before deciding defaults:
`MC2_OVERLAY_TILE_HIRES=1` vs sidecar gate, same camera, crop compare.
They compose (different layers), so "both on" is also a legal outcome.

## 5. Known noise / tooling gaps hit during validation

- `scripts/check-new-gates.py` FAILs on this branch with 45 UNREGISTERED gates —
  ALL inherited from the base lineage (Mode A base `dc8308c0`);
  `MC2_OVERLAY_TILE_HIRES` itself IS registered (RendererFeatureRegistry
  kAuxEnvVars). Don't let the red scare the merge.
- `scripts/slice_gate.py` can't target the v0.3 spare lane by name
  (`DEPLOY_FOLDERS` in `scripts/smoke_lib/deploy_lease.py` has no 0.3 entry);
  validation used `run_smoke.py --exe A:/Games/mc2-opengl/mc2-win64-v0.3/mc2.exe`
  pair (baseline + gate-ON) instead. Optional cleanup: add a `0.3` lane row.
- Gate must stay on the run_smoke env allowlist (commit `f7b872e2`) or Popen
  drops it and a "gate-ON" smoke silently runs the legacy path.
